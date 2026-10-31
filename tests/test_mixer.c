/* White-box tests of the DJ mixer and deck loops. The mixer's device runs on the
 * null backend and is stopped right after it is made, so this test plays the part
 * of the audio thread (fa_device_render_output) and checks the samples. Deck
 * loops are checked sample by sample on a plain device, where the mix is exact
 * (the mixer's isolator is an allpass, so it moves samples). */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "mixer.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__APPLE__)
#  include <TargetConditionals.h>
#endif

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; return; } } while (0)
#define CHECK_EQ(a, b) do { long long va_ = (long long)(a), vb_ = (long long)(b); if (va_ != vb_) { \
    fprintf(stderr, "  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); g_failures++; return; } } while (0)
#define CHECK_NEAR(a, b, tol) do { double va_ = (a), vb_ = (b); if (fabs(va_ - vb_) > (tol)) { \
    fprintf(stderr, "  FAIL %s:%d: %s ~ %s (%.6f vs %.6f)\n", __FILE__, __LINE__, #a, #b, va_, vb_); g_failures++; return; } } while (0)
#define RUN(t) do { int before_ = g_failures; printf("%s\n", #t); t(); if (g_failures == before_) printf("  ok\n"); } while (0)

static fa_engine* g_engine;
static fa_device* g_dev;     /* plain device for the loop tests, never started */
static uint32_t g_rate;

/* ---- media ------------------------------------------------------------------ */

/* 16-bit mono WAV at `rate`. kind 0: a sawtooth whose value encodes the frame index
 * modulo 4096 (exact in float); kind 1: a sine of `hz` at amplitude 0.25. */
static uint8_t* make_wav(uint32_t rate, uint32_t frames, int kind, double hz, size_t* size)
{
    size_t data = (size_t)frames * 2, n = 44 + data;
    uint8_t* b = (uint8_t*)malloc(n);
    uint32_t i, v;
    memcpy(b, "RIFF", 4); v = (uint32_t)(n - 8); memcpy(b + 4, &v, 4); memcpy(b + 8, "WAVEfmt ", 8);
    v = 16; memcpy(b + 16, &v, 4);
    b[20] = 1; b[21] = 0; b[22] = 1; b[23] = 0;
    memcpy(b + 24, &rate, 4); v = rate * 2; memcpy(b + 28, &v, 4);
    b[32] = 2; b[33] = 0; b[34] = 16; b[35] = 0;
    memcpy(b + 36, "data", 4); v = (uint32_t)data; memcpy(b + 40, &v, 4);
    for (i = 0; i < frames; i++) {
        int16_t sv = kind == 0 ? (int16_t)((int)(i % 4096) * 8 - 16384)
                               : (int16_t)lrint(8192.0 * sin(2.0 * 3.14159265358979 * hz * i / rate));
        memcpy(b + 44 + (size_t)i * 2, &sv, 2);
    }
    *size = n;
    return b;
}

static fa_stream* open_wav(uint32_t frames, int kind, double hz)
{
    size_t n;
    uint8_t* wav = make_wav(g_rate, frames, kind, hz, &n);
    fa_stream* s = NULL;
    if (fa_stream_open_memory(g_engine, wav, n, NULL, &s) != FA_OK) s = NULL;
    free(wav);
    return s;
}

/* The sawtooth's frame index (mod 4096) of a sample. */
static int saw_index(float v) { return (int)lrintf((v * 32768.0f + 16384.0f) / 8.0f); }

/* Acts as the audio callback of the plain device for one stream. */
static void render(fa_stream* s, float* out, uint32_t frames)
{
    int i;
    for (i = 0; i < 2000; i++) {
        if (fa_ring_readable(&s->ring) >= (int64_t)frames || fa_load32(&s->eof)) break;
        fa_sleep_ms(1);
    }
    memset(out, 0, (size_t)frames * 2 * sizeof(float));
    fa_fetch_add64(&g_dev->epoch, 1);
    fa_stream_mix(s, g_dev, out, frames);
    fa_fetch_add64(&g_dev->clock, frames);
    fa_fetch_add64(&g_dev->epoch, 1);
}

/* Renders `frames` and appends the sawtooth indices (left channel) to idx[*n]. */
static void render_saw(fa_stream* s, uint32_t frames, int* idx, int* n)
{
    float out[2 * 512];
    while (frames > 0) {
        uint32_t k = frames < 512 ? frames : 512, i;
        render(s, out, k);
        for (i = 0; i < k; i++) idx[(*n)++] = saw_index(out[i * 2]);
        frames -= k;
    }
}

/* Index of the first place where idx stops counting up by one (mod 4096), from `from`. */
static int first_jump(const int* idx, int n, int from)
{
    int i;
    for (i = from + 1; i < n; i++)
        if (idx[i] != (idx[i - 1] + 1) % 4096) return i;
    return -1;
}

/* I/O over a memory WAV that counts seeks and fails them on request. */
typedef struct mem_io {
    uint8_t* data;
    int64_t size, pos;
    fa_atomic32 seeks, fail_seeks;
} mem_io;

static int64_t FA_CALL mio_read(void* user, void* buffer, size_t bytes)
{
    mem_io* m = (mem_io*)user;
    int64_t n = m->size - m->pos;
    if (n > (int64_t)bytes) n = (int64_t)bytes;
    memcpy(buffer, m->data + m->pos, (size_t)n);
    m->pos += n;
    return n;
}

static fa_result FA_CALL mio_seek(void* user, int64_t offset)
{
    mem_io* m = (mem_io*)user;
    fa_fetch_add32(&m->seeks, 1);
    if (fa_load32(&m->fail_seeks) || offset < 0 || offset > m->size) return FA_ERROR_IO;
    m->pos = offset;
    return FA_OK;
}

static int64_t FA_CALL mio_length(void* user) { return ((mem_io*)user)->size; }

/* A sawtooth stream reading through `m` (freed by the caller after the close). */
static fa_stream* open_saw_io(mem_io* m, uint32_t frames)
{
    fa_io_callbacks io;
    size_t n;
    fa_stream* s = NULL;
    memset(m, 0, sizeof(*m));
    memset(&io, 0, sizeof(io));
    m->data = make_wav(g_rate, frames, 0, 0.0, &n);
    m->size = (int64_t)n;
    io.read = mio_read;
    io.seek = mio_seek;
    io.length = mio_length;
    if (fa_stream_open_io(g_engine, &io, m, NULL, &s) != FA_OK) s = NULL;
    return s;
}

/* ---- events ----------------------------------------------------------------- */

typedef struct recorder {
    fa_mutex lock;
    int looped;
} recorder;

static void FA_CALL record(const fa_event* ev, void* user)
{
    recorder* r = (recorder*)user;
    fa_mutex_lock(&r->lock);
    if (ev->type == FA_EVENT_LOOPED) r->looped++;
    fa_mutex_unlock(&r->lock);
}

static int looped_events(recorder* r, int wait_for)
{
    int n = 0, tries;
    for (tries = 0; tries < 200; tries++) {
        fa_mutex_lock(&r->lock);
        n = r->looped;
        fa_mutex_unlock(&r->lock);
        if (n >= wait_for) break;
        fa_sleep_ms(5);
    }
    return n;
}

/* ---- deck loops (plain device, exact samples) --------------------------------- */

static fa_stream* playing_saw(uint32_t frames)
{
    fa_stream* s = open_wav(frames, 0, 0.0);
    if (!s) return NULL;
    if (fa_stream_attach(s, g_dev, NULL) != FA_OK || fa_stream_play(s) != FA_OK) { fa_stream_close(s); return NULL; }
    return s;
}

/* A loop whose out point the decoder has not reached yet is taken where it decodes. */
static void test_loop_ahead_of_decoder(void)
{
    fa_stream* s = playing_saw(400000);
    int* idx = (int*)malloc(sizeof(int) * 300000), n = 0, j;
    fa_frames pos = 0;
    recorder rec;
    CHECK(s && idx);
    memset(&rec, 0, sizeof(rec));
    fa_mutex_init(&rec.lock);
    fa_stream_set_event_callback(s, record, &rec);
    render_saw(s, 1024, idx, &n);
    CHECK_EQ(fa_stream_set_loop_region(s, 100000, 120000), FA_OK);
    render_saw(s, 140000, idx, &n);
    /* 0..119999 straight, then back to 100000. */
    j = first_jump(idx, n, 0);
    CHECK_EQ(j, 120000);
    CHECK_EQ(idx[j], 100000 % 4096);
    CHECK_EQ(first_jump(idx, n, j), 140000);   /* the second turn */
    CHECK(looped_events(&rec, 2) >= 2);
    fa_stream_get_position(s, &pos);
    CHECK(pos >= 100000 && pos < 120000);
    fa_stream_set_event_callback(s, NULL, NULL);
    fa_stream_close(s);
    fa_mutex_destroy(&rec.lock);
    free(idx);
}

/* An out point inside the decoded-ahead audio but ahead of the mix: joined exactly there. */
static void test_loop_out_inside_ring(void)
{
    fa_stream* s = playing_saw(200000);
    int idx[20000], n = 0, j, i;
    CHECK(s);
    render_saw(s, 2048, idx, &n);
    for (i = 0; i < 2000 && fa_ring_readable(&s->ring) < 8000; i++) fa_sleep_ms(1);
    CHECK(fa_ring_readable(&s->ring) >= 8000);           /* decoded well past the out point */
    CHECK_EQ(fa_stream_set_loop_region(s, 500, 3500), FA_OK);
    render_saw(s, 8000, idx, &n);
    j = first_jump(idx, n, 0);
    CHECK_EQ(j, 3500);
    CHECK_EQ(idx[j], 500);
    CHECK_EQ(first_jump(idx, n, j), 3500 + 3000);
    fa_stream_close(s);
}

/* An out point the mix just passed ("loop out" pressed late): back into the loop at
 * once, at the phase it would have had. */
static void test_loop_out_just_passed(void)
{
    fa_stream* s = playing_saw(200000);
    int idx[20000], n = 0, j, margin = (int)(g_rate * 15 / 1000), want, i;
    CHECK(s);
    render_saw(s, 3000, idx, &n);
    /* The join is a margin ahead of the mix only once the deck has decoded that far. */
    for (i = 0; i < 2000 && fa_ring_readable(&s->ring) < (uint32_t)margin + 512; i++) fa_sleep_ms(1);
    CHECK_EQ(fa_stream_set_loop_region(s, 500, 2800), FA_OK);
    render_saw(s, 6000, idx, &n);
    j = first_jump(idx, n, 0);
    CHECK_EQ(j, 3000 + margin);                       /* the join, a margin ahead of the mix */
    want = 500 + (3000 + margin - 2800) % 2300;
    CHECK_EQ(idx[j], want);
    CHECK_EQ(idx[first_jump(idx, n, j)], 500);         /* and loops at 2800 from then on */
    fa_stream_close(s);
}

/* An out point far behind: to the in point. */
static void test_loop_far_behind_goes_to_in(void)
{
    fa_stream* s = playing_saw(200000);
    int* idx = (int*)malloc(sizeof(int) * 80000), n = 0, j, margin = (int)(g_rate * 15 / 1000), i;
    fa_frames pos = 0;
    CHECK(s && idx);
    render_saw(s, 60000, idx, &n);
    for (i = 0; i < 2000 && fa_ring_readable(&s->ring) < (uint32_t)margin + 512; i++) fa_sleep_ms(1);
    CHECK_EQ(fa_stream_set_loop_region(s, 500, 2500), FA_OK);
    render_saw(s, 4000, idx, &n);
    j = first_jump(idx, n, 0);
    CHECK_EQ(j, 60000 + margin);
    CHECK_EQ(idx[j], 500);
    fa_stream_get_position(s, &pos);
    CHECK(pos >= 500 && pos < 2500);
    fa_stream_close(s);
    free(idx);
}

/* Turning the loop off after it has turned carries on from where it is, past the
 * old out point. */
static void test_loop_off_carries_on(void)
{
    fa_stream* s = playing_saw(200000);
    int idx[30000], n = 0, j, margin = (int)(g_rate * 15 / 1000);
    fa_frames in = -1, out = -1;
    CHECK(s);
    CHECK_EQ(fa_stream_set_loop_region(s, 500, 2500), FA_OK);
    render_saw(s, 3000, idx, &n);                      /* turned once: at 1000 now */
    CHECK_EQ(first_jump(idx, n, 0), 2500);
    CHECK_EQ(fa_stream_set_loop_region(s, 0, 0), FA_OK);
    fa_stream_get_loop_region(s, &in, &out);
    CHECK_EQ(out, 0);
    render_saw(s, 6000, idx, &n);
    /* The join (margin ahead) continues the count, and 2500 passes without a turn. */
    j = first_jump(idx, n, 2600);
    CHECK_EQ(j, -1);
    CHECK_EQ(idx[n - 1], (1000 + 6000 - 1) % 4096);
    (void)margin;
    fa_stream_close(s);
}

/* A seek past the out point leaves the loop. */
static void test_seek_past_loop_leaves_it(void)
{
    fa_stream* s = playing_saw(200000);
    int idx[8000], n = 0;
    fa_frames in = -1, out = -1;
    CHECK(s);
    CHECK_EQ(fa_stream_set_loop_region(s, 500, 2500), FA_OK);
    render_saw(s, 1000, idx, &n);
    CHECK_EQ(fa_stream_seek(s, 50000), FA_OK);
    fa_stream_get_loop_region(s, &in, &out);
    CHECK_EQ(out, 0);
    n = 0;
    render_saw(s, 4000, idx, &n);
    CHECK_EQ(idx[0], 50000 % 4096);
    CHECK_EQ(first_jump(idx, n, 0), -1);
    fa_stream_close(s);
}

/* Moving a playing loop (a beat jump with a loop on) is one join: the jump and the
 * moved loop start together, a margin ahead of the mix, and the loop turns there. A
 * call that cannot join leaves both the position and the loop alone. */
static void test_jump_moves_loop(void)
{
    fa_stream* s = playing_saw(400000);
    int* idx = (int*)malloc(sizeof(int) * 80000);
    int n = 0, n0, j, j2, margin = (int)(g_rate * 15 / 1000);
    fa_frames in = -1, out = -1, a;
    CHECK(s && idx);
    CHECK_EQ(fa_stream_set_loop_region(s, 20000, 30000), FA_OK);
    render_saw(s, 35000, idx, &n);                     /* turned once: at 25000 now */
    CHECK_EQ(first_jump(idx, n, 0), 30000);
    CHECK_EQ(fa_stream_jump_loop(s, 50000, 70000, 80000), FA_OK);
    fa_stream_get_loop_region(s, &in, &out);
    CHECK_EQ(in, 70000);
    CHECK_EQ(out, 80000);
    n0 = n;
    render_saw(s, 30000, idx, &n);
    j = first_jump(idx, n, n0 - 1);
    CHECK(j >= n0 && j <= n0 + margin + 1);
    a = 20000 + (j - 30000) % 10000;                   /* where the old loop played at the join */
    CHECK_EQ(idx[j], (a + 50000) % 4096);
    j2 = first_jump(idx, n, j);                        /* then the moved loop turns */
    CHECK_EQ(j2, j + (80000 - (a + 50000)));
    CHECK_EQ(idx[j2], 70000 % 4096);
    CHECK_EQ(fa_stream_jump_loop(s, 1000, 90000, 90000), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_stream_jump_loop(s, 1000, 390000, 400001), FA_ERROR_INVALID_ARG);   /* past the end */
    CHECK_EQ(fa_stream_detach(s), FA_OK);
    CHECK_EQ(fa_stream_jump_loop(s, 1000, 1000, 2000), FA_ERROR_BUSY);   /* not playing: nothing moves */
    fa_stream_get_loop_region(s, &in, &out);
    CHECK_EQ(in, 70000);
    CHECK_EQ(out, 80000);
    fa_stream_close(s);
    free(idx);
}

/* ---- deck loops and jumps, reversed ------------------------------------------- */

/* Playing backwards from `from`. */
static fa_stream* reversed_saw(uint32_t frames, fa_frames from)
{
    fa_stream* s = open_wav(frames, 0, 0.0);
    if (!s) return NULL;
    if (fa_stream_attach(s, g_dev, NULL) != FA_OK || fa_stream_seek(s, from) != FA_OK ||
        fa_stream_set_reverse(s, 1) != FA_OK || fa_stream_play(s) != FA_OK) {
        fa_stream_close(s);
        return NULL;
    }
    return s;
}

/* Index of the first place where idx stops counting down by one (mod 4096), from `from`. */
static int first_jump_down(const int* idx, int n, int from)
{
    int i;
    for (i = from + 1; i < n; i++)
        if (idx[i] != (idx[i - 1] + 4095) % 4096) return i;
    return -1;
}

static void wait_decoded(fa_stream* s, int64_t frames)
{
    int i;
    for (i = 0; i < 2000 && fa_ring_readable(&s->ring) < frames; i++) fa_sleep_ms(1);
}

/* Reversed, a deck loop turns at its in point, back up to its out point, without a
 * gap: one the decoder has not reached yet is taken where it decodes, one whose in
 * point is in the decoded-ahead audio is joined exactly there. LOOPED is raised at
 * each turn, with the in point it left and the out point it went to. */
static void test_reverse_loop_joins(void)
{
    fa_stream* s = reversed_saw(400000, 300000);
    int* idx = (int*)malloc(sizeof(int) * 300000), n = 0, j;
    fa_frames pos = 0;
    recorder rec;
    CHECK(s && idx);
    memset(&rec, 0, sizeof(rec));
    fa_mutex_init(&rec.lock);
    fa_stream_set_event_callback(s, record, &rec);
    render_saw(s, 1024, idx, &n);
    CHECK_EQ(fa_stream_set_loop_region(s, 180000, 200000), FA_OK);   /* ahead of the decoder */
    render_saw(s, 140000, idx, &n);
    /* 299999..180000 straight, then up to 199999. */
    j = first_jump_down(idx, n, 0);
    CHECK_EQ(j, 120000);
    CHECK_EQ(idx[j - 1], 180000 % 4096);
    CHECK_EQ(idx[j], 199999 % 4096);
    CHECK_EQ(first_jump_down(idx, n, j), 140000);   /* the second turn */
    CHECK(looped_events(&rec, 2) >= 2);
    fa_stream_get_position(s, &pos);
    CHECK(pos >= 180000 && pos < 200000);
    fa_stream_set_event_callback(s, NULL, NULL);
    fa_stream_close(s);
    fa_mutex_destroy(&rec.lock);

    /* The in point inside the decoded-ahead audio: joined exactly there. */
    s = reversed_saw(200000, 200000);
    n = 0;
    CHECK(s);
    render_saw(s, 2048, idx, &n);
    wait_decoded(s, 8000);
    CHECK(fa_ring_readable(&s->ring) >= 8000);
    CHECK_EQ(fa_stream_set_loop_region(s, 194500, 197500), FA_OK);
    render_saw(s, 12000, idx, &n);
    j = first_jump_down(idx, n, 0);
    CHECK_EQ(j, 5500);
    CHECK_EQ(idx[j], 197499 % 4096);
    CHECK_EQ(first_jump_down(idx, n, j), 5500 + 3000);
    fa_stream_close(s);
    free(idx);
}

/* Reversed, an in point the mix just passed loops back at once at the phase it would
 * have had; one far behind goes to the out point; turning the loop off carries on
 * down past the old in point. */
static void test_reverse_loop_late_and_off(void)
{
    fa_stream* s = reversed_saw(200000, 200000);
    int* idx = (int*)malloc(sizeof(int) * 80000);
    int n = 0, j, margin = (int)(g_rate * 15 / 1000), a;
    CHECK(s && idx);
    render_saw(s, 3000, idx, &n);                       /* at 197000 */
    wait_decoded(s, margin + 512);
    CHECK_EQ(fa_stream_set_loop_region(s, 197200, 199500), FA_OK);
    render_saw(s, 6000, idx, &n);
    j = first_jump_down(idx, n, 0);
    CHECK_EQ(j, 3000 + margin);                         /* the join, a margin ahead of the mix */
    a = 200000 - (3000 + margin);                       /* the frame heard last before it */
    CHECK_EQ(idx[j], (199500 - (197200 - a) % 2300 - 1) % 4096);
    CHECK_EQ(idx[first_jump_down(idx, n, j)], 199499 % 4096);   /* turns at 197200 from then on */
    fa_stream_close(s);

    s = reversed_saw(200000, 160000);                   /* far behind: to the out point */
    n = 0;
    CHECK(s);
    render_saw(s, 20000, idx, &n);
    wait_decoded(s, margin + 512);
    CHECK_EQ(fa_stream_set_loop_region(s, 197500, 199500), FA_OK);
    render_saw(s, 4000, idx, &n);
    j = first_jump_down(idx, n, 0);
    CHECK_EQ(j, 20000 + margin);
    CHECK_EQ(idx[j], 199499 % 4096);
    fa_stream_close(s);

    s = reversed_saw(200000, 200000);                   /* off after it turned */
    n = 0;
    CHECK(s);
    CHECK_EQ(fa_stream_set_loop_region(s, 197500, 199500), FA_OK);
    render_saw(s, 3000, idx, &n);                       /* turned once: at 199000 */
    CHECK_EQ(first_jump_down(idx, n, 0), 2500);
    CHECK_EQ(fa_stream_set_loop_region(s, 0, 0), FA_OK);
    render_saw(s, 6000, idx, &n);
    CHECK_EQ(first_jump_down(idx, n, 2600), -1);        /* 197500 passes without a turn */
    CHECK_EQ(idx[n - 1], (199000 - 6000) % 4096);
    fa_stream_close(s);
    free(idx);
}

/* Reversed, fa_stream_jump joins without a gap: a margin ahead of the mix, or where
 * playback (going down) reaches `at`; playback goes on down from there plus delta. */
static void test_reverse_jump(void)
{
    fa_stream* s = reversed_saw(400000, 300000);
    int* idx = (int*)malloc(sizeof(int) * 40000);
    int n = 0, j, margin = (int)(g_rate * 15 / 1000);
    fa_result r = FA_ERROR_BUSY;
    fa_frames pos = 0;
    CHECK(s && idx);
    render_saw(s, 1000, idx, &n);
    wait_decoded(s, margin + 512);
    CHECK_EQ(fa_stream_jump(s, -1, -20000), FA_OK);
    render_saw(s, 4000, idx, &n);
    j = first_jump_down(idx, n, 0);
    CHECK_EQ(j, 1000 + margin);
    CHECK_EQ(idx[j], (300000 - (1000 + margin) - 1 - 20000) % 4096);
    CHECK_EQ(first_jump_down(idx, n, j), -1);
    fa_stream_get_position(s, &pos);
    CHECK_EQ(pos, 300000 - 20000 - 5000);
    /* At a frame ahead: where frame 270000 is heard, on from 271999 down. */
    while (r == FA_ERROR_BUSY && n < 30000) {
        r = fa_stream_jump(s, 270000, 2000);
        render_saw(s, 512, idx, &n);
    }
    CHECK_EQ(r, FA_OK);
    render_saw(s, 10000, idx, &n);
    j = first_jump_down(idx, n, 5000);
    CHECK_EQ(idx[j - 1], 270000 % 4096);
    CHECK_EQ(idx[j], 271999 % 4096);
    CHECK_EQ(first_jump_down(idx, n, j), -1);
    fa_stream_close(s);
    free(idx);
}

/* ---- instant reverse ------------------------------------------------------------ */

/* Turning while playing: the audio just heard plays back the other way at once, out of
 * the ring, then the decoder carries on where it ends. Every frame steps by one; the
 * turn itself plays the frame heard last again (the mirror). Both ways, with no flush. */
static void test_turn_instant(void)
{
    static mem_io m;   /* outlives a failed CHECK's early return (the stream reads it) */
    fa_stream* s = open_saw_io(&m, 400000);
    int* idx = (int*)malloc(sizeof(int) * 200000);
    int n = 0, k, hist = (int)(g_rate * 200 / 1000);
    fa_frames pos = 0;
    CHECK(s && idx);
    CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
    CHECK_EQ(fa_stream_seek(s, 100000), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    render_saw(s, 20000, idx, &n);                      /* heard up to 119999 */
    CHECK_EQ(fa_stream_set_reverse(s, 1), FA_OK);
    CHECK_EQ(fa_load32(&s->flushing), 0);
    CHECK(fa_ring_readable(&s->ring) >= hist);
    fa_stream_get_position(s, &pos);
    CHECK_EQ(pos, 120000);
    render_saw(s, 60000, idx, &n);
    CHECK_EQ(idx[20000], 119999 % 4096);
    CHECK_EQ(first_jump_down(idx, n, 20000), -1);
    fa_stream_get_position(s, &pos);
    CHECK_EQ(pos, 60000);
    CHECK_EQ(fa_stream_set_reverse(s, 0), FA_OK);
    k = n;
    render_saw(s, 60000, idx, &n);
    CHECK_EQ(idx[k], 60000 % 4096);
    CHECK_EQ(first_jump(idx, n, k), -1);
    fa_stream_get_position(s, &pos);
    CHECK_EQ(pos, 120000);
    /* The history needs no decoder: with every seek failing it still plays, then the
     * stream ends where it runs out. */
    fa_store32(&m.fail_seeks, 1);
    fa_stream_set_reverse(s, 1);
    k = n;
    render_saw(s, (uint32_t)hist + 2000, idx, &n);
    CHECK_EQ(idx[k], 119999 % 4096);
    CHECK_EQ(first_jump_down(idx, n, k), k + hist);
    fa_stream_close(s);
    free(idx);
}

/* Inside a deck loop the history stops where the loop last turned (what played before
 * came from its out point); reversed, the loop then turns at its in point. */
static void test_turn_in_loop(void)
{
    fa_stream* s = open_wav(400000, 0, 0.0);
    int* idx = (int*)malloc(sizeof(int) * 80000);
    int n = 0, k;
    CHECK(s && idx);
    CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    CHECK_EQ(fa_stream_set_loop_region(s, 10000, 30000), FA_OK);
    render_saw(s, 33000, idx, &n);                      /* turned at 30000: heard 10000..12999 */
    CHECK_EQ(first_jump(idx, n, 0), 30000);
    CHECK_EQ(fa_stream_set_reverse(s, 1), FA_OK);
    k = n;
    render_saw(s, 30000, idx, &n);
    CHECK_EQ(idx[k], 12999 % 4096);
    CHECK_EQ(first_jump_down(idx, n, k), k + 3000);     /* down to 10000, then from 29999 */
    CHECK_EQ(idx[k + 3000], 29999 % 4096);
    CHECK_EQ(first_jump_down(idx, n, k + 3000), k + 23000);
    fa_stream_close(s);
    free(idx);
}

/* Largest step between neighbouring samples (left channel) of out[from, to). */
static double max_step(const float* out, int from, int to)
{
    double m = 0.0;
    int i;
    for (i = from + 1; i < to; i++) {
        double d = fabs((double)out[i * 2] - (double)out[(i - 1) * 2]);
        if (d > m) m = d;
    }
    return m;
}

/* A sine turned while playing: no step at the turn, nor where the history meets the
 * decoder, above the sine's own. Through the stretcher (TEMPO 1.25) that second join is
 * a cut, like a loop's (the stretcher starts over there), but nothing goes silent; the
 * position counts down by the frames played x TEMPO both ways. */
static void test_turn_smooth(void)
{
    static const float tempos[] = { 1.0f, 1.25f };
    float* out = (float*)malloc(sizeof(float) * 2 * 2 * 48128);
    int t;
    CHECK(out);
    for (t = 0; t < 2; t++) {
        fa_stream* s = open_wav(g_rate * 6, 1, 440.0);
        fa_frames p0 = 0, p1 = 0;
        int i, half = 48128;
        CHECK(s);
        CHECK_EQ(fa_stream_set_param(s, FA_PARAM_TEMPO, tempos[t]), FA_OK);
        CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
        CHECK_EQ(fa_stream_seek(s, g_rate * 3), FA_OK);
        CHECK_EQ(fa_stream_play(s), FA_OK);
        for (i = 0; i < half; i += 512) render(s, out + (size_t)i * 2, 512);
        fa_stream_get_position(s, &p0);
        CHECK_EQ(fa_stream_set_reverse(s, 1), FA_OK);
        for (i = half; i < 2 * half; i += 512) render(s, out + (size_t)i * 2, 512);
        fa_stream_get_position(s, &p1);
        if (t == 0) {
            double own = max_step(out, 2000, half);      /* the sine's own, steady */
            CHECK(own > 0.01);
            CHECK(max_step(out, half - 2000, 2 * half) < own * 1.01);
        }
        for (i = half - 2000; i + 128 <= 2 * half; i += 64) {   /* no silent stretch: 128-frame peaks */
            double pk = 0.0;
            int j;
            for (j = i; j < i + 128; j++) if (fabs(out[j * 2]) > pk) pk = fabs(out[j * 2]);
            if (pk < 0.2) break;
        }
        CHECK(i + 128 > 2 * half);
        CHECK_NEAR((double)p1, (double)p0 - half * tempos[t], 2.0);
        fa_stream_close(s);
    }
    free(out);
}

/* A loop change or a jump whose join fails (the decoder cannot seek) reports the
 * error and leaves the loop as it was. */
static void test_failed_join_keeps_loop(void)
{
    static mem_io m;   /* outlives a failed CHECK's early return (the stream reads it) */
    fa_stream* s = open_saw_io(&m, 400000);
    int* idx = (int*)malloc(sizeof(int) * 40000);
    int n = 0, i;
    fa_frames in = -1, out = -1;
    CHECK(s && idx);
    CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    CHECK_EQ(fa_stream_set_loop_region(s, 20000, 30000), FA_OK);
    render_saw(s, 35000, idx, &n);                     /* turned once */
    CHECK_EQ(first_jump(idx, n, 0), 30000);
    fa_store32(&m.fail_seeks, 1);
    CHECK(fa_stream_jump_loop(s, 50000, 70000, 80000) != FA_OK);
    fa_stream_get_loop_region(s, &in, &out);
    CHECK_EQ(in, 20000);
    CHECK_EQ(out, 30000);
    fa_stream_close(s);
    free(m.data);

    s = open_saw_io(&m, 200000);
    CHECK(s);
    CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    n = 0;
    render_saw(s, 30000, idx, &n);
    for (i = 0; i < 2000 && fa_ring_readable(&s->ring) < 8000; i++) fa_sleep_ms(1);
    CHECK(fa_ring_readable(&s->ring) >= 8000);         /* decoded past the out point: joins */
    fa_store32(&m.fail_seeks, 1);
    /* In point well past the start: the first few KB stay buffered and seek without I/O. */
    CHECK(fa_stream_set_loop_region(s, 20000, 33000) != FA_OK);
    fa_stream_get_loop_region(s, &in, &out);
    CHECK_EQ(in, 0);
    CHECK_EQ(out, 0);
    fa_stream_close(s);
    free(m.data);
    free(idx);
}

/* The audio thread reads the deck loop while it changes back to back: every reading
 * is one whole setting (run under TSan, the old reader was a data race). */
typedef struct loop_flipper {
    fa_stream* s;
    fa_atomic32 done;
    int errors;
} loop_flipper;

static void flip_loops(void* arg)
{
    loop_flipper* f = (loop_flipper*)arg;
    int i;
    for (i = 0; i < 200; i++) {
        fa_frames in = (fa_frames)(1 + i % 8) * 10000;
        if (fa_stream_set_loop_region(f->s, in, in + 5000) != FA_OK) f->errors++;
    }
    fa_store32(&f->done, 1);
}

static void test_loop_region_readers(void)
{
    loop_flipper f;
    fa_thread th;
    float out[2 * 256];
    int rounds = 0, torn = 0;
    memset(&f, 0, sizeof(f));
    f.s = playing_saw(400000);
    CHECK(f.s);
    render(f.s, out, 256);
    CHECK_EQ(fa_thread_create(&th, flip_loops, &f), 0);
    while (!fa_load32(&f.done) || rounds < 100) {
        fa_frames in = -1, o = -1;
        fa_fetch_add64(&g_dev->epoch, 1);
        fa_stream_mix(f.s, g_dev, out, 256);
        fa_fetch_add64(&g_dev->clock, 256);
        fa_fetch_add64(&g_dev->epoch, 1);
        fa_stream_get_loop_region(f.s, &in, &o);
        if (o - in != 5000 && (in != 0 || o != 0)) torn++;   /* (0, 0) before the first change */
        rounds++;
        if (rounds % 8 == 0) fa_sleep_ms(1);
    }
    fa_thread_join(&th);
    CHECK_EQ(f.errors, 0);
    CHECK_EQ(torn, 0);
    fa_stream_close(f.s);
}

/* Loops follow tempo: with the stretcher engaged the position stays in the loop. */
static void test_loop_with_tempo(void)
{
    fa_stream* s = playing_saw(400000);
    float out[2 * 512];
    fa_frames pos = 0;
    int i, inside = 1;
    CHECK(s);
    CHECK_EQ(fa_stream_set_param(s, FA_PARAM_TEMPO, 1.25f), FA_OK);
    for (i = 0; i < 40; i++) render(s, out, 512);
    CHECK_EQ(fa_stream_set_loop_region(s, 10000, 30000), FA_OK);
    for (i = 0; i < 400; i++) {
        render(s, out, 512);
        fa_stream_get_position(s, &pos);
        if (i > 100 && (pos < 10000 || pos >= 30000)) inside = 0;
    }
    CHECK(inside);
    fa_stream_close(s);
}

/* ---- the mixer -------------------------------------------------------------- */

typedef struct rig {
    fa_mixer* m;
    fa_device* dev;
    fa_deck* a;
    fa_deck* b;
    uint32_t channels;
} rig;

static int rig_open(rig* r, fa_cue_mode cue)
{
    fa_mixer_config c;
    memset(r, 0, sizeof(*r));
    fa_mixer_config_init(&c);
    c.cue_mode = cue;
    c.latency = FA_LATENCY_NORMAL;
    if (fa_mixer_create(g_engine, &c, &r->m) != FA_OK) return 0;
    fa_mixer_get_device(r->m, &r->dev);
    fa_device_stop(r->dev);   /* this test is the audio thread */
    fa_mixer_get_deck(r->m, 0, &r->a);
    fa_mixer_get_deck(r->m, 1, &r->b);
    fa_mixer_get_output_channels(r->m, &r->channels, NULL);
    return 1;
}

/* Renders `frames` of the mixer's output (all channels) after the decks' workers
 * have that much decoded. */
static void rig_render(rig* r, float* out, uint32_t frames)
{
    int i, d;
    for (i = 0; i < 2000; i++) {
        int ok = 1;
        for (d = 0; d < 2; d++) {
            fa_stream* s = NULL;
            fa_deck_get_stream(d ? r->b : r->a, &s);
            if (s && fa_ring_readable(&s->ring) < (int64_t)frames && !fa_load32(&s->eof)) ok = 0;
        }
        if (ok) break;
        fa_sleep_ms(1);
    }
    fa_fetch_add64(&r->dev->epoch, 1);
    fa_device_render_output(r->dev, out, r->channels, frames, fa_monotonic_ns());
    fa_fetch_add64(&r->dev->epoch, 1);
}

/* RMS of channel `c` over the last `n` frames of `out` (frames long). */
static double rms_of(const float* out, uint32_t channels, uint32_t frames, uint32_t n, uint32_t c)
{
    double sq = 0.0;
    uint32_t i;
    for (i = frames - n; i < frames; i++) sq += (double)out[i * channels + c] * out[i * channels + c];
    return sqrt(sq / n);
}

/* Plays a sine of `hz` on deck A and returns the steady RMS of output channel `c`. */
static double sine_rms(rig* r, uint32_t c, float* buf, uint32_t frames)
{
    uint32_t done;
    for (done = 0; done + 1024 <= frames; done += 1024) rig_render(r, buf + (size_t)done * r->channels, 1024);
    return rms_of(buf, r->channels, frames, frames / 2, c);
}

static fa_stream* load_sine(fa_deck* d, double hz)
{
    fa_stream* s = open_wav(g_rate * 4, 1, hz);
    if (!s) return NULL;
    if (fa_deck_load(d, s, 0) != FA_OK || fa_stream_play(s) != FA_OK) { fa_stream_close(s); return NULL; }
    return s;
}

static const double k_sine_rms = 0.25 / 1.41421356;   /* the test sine's own RMS */

/* At 0 dB the isolator's bands sum flat; the crossfader at A passes deck A whole. */
static void test_isolator_flat(void)
{
    static const double hz[] = { 60.0, 250.0, 1000.0, 2500.0, 8000.0 };
    rig r;
    float* buf = (float*)malloc(sizeof(float) * 2 * 24576);
    int i;
    CHECK(buf && rig_open(&r, FA_CUE_NONE));
    CHECK_EQ(r.channels, 2);
    CHECK_EQ(fa_mixer_set_param(r.m, FA_MIX_CROSSFADER, 0.0f), FA_OK);
    for (i = 0; i < 5; i++) {
        fa_stream* s = load_sine(r.a, hz[i]);
        CHECK(s);
        CHECK_NEAR(sine_rms(&r, 0, buf, 24576) / k_sine_rms, 1.0, 0.01);   /* within 0.1 dB */
        CHECK_EQ(fa_deck_load(r.a, NULL, FA_DECK_LOAD_FORCE), FA_OK);
        fa_stream_close(s);
    }
    fa_mixer_destroy(r.m);
    free(buf);
}

/* Kills: a killed band is gone, the others stay. */
static void test_isolator_kills(void)
{
    rig r;
    float* buf = (float*)malloc(sizeof(float) * 2 * 24576);
    fa_stream* s;
    CHECK(buf && rig_open(&r, FA_CUE_NONE));
    fa_mixer_set_param(r.m, FA_MIX_CROSSFADER, 0.0f);
    CHECK_EQ(fa_deck_set_param(r.a, FA_DECK_EQ_LOW_DB, -100.0f), FA_OK);   /* clamped to the kill */
    s = load_sine(r.a, 60.0);
    CHECK(s);
    CHECK(sine_rms(&r, 0, buf, 24576) / k_sine_rms < 0.01);           /* below -40 dB */
    fa_deck_load(r.a, NULL, FA_DECK_LOAD_FORCE);
    fa_stream_close(s);
    s = load_sine(r.a, 8000.0);                                              /* the highs untouched */
    CHECK(s);
    CHECK_NEAR(sine_rms(&r, 0, buf, 24576) / k_sine_rms, 1.0, 0.02);
    CHECK_EQ(fa_deck_set_param(r.a, FA_DECK_EQ_HIGH_DB, FA_DECK_EQ_KILL_DB), FA_OK);
    CHECK(sine_rms(&r, 0, buf, 24576) / k_sine_rms < 0.01);
    CHECK_EQ(fa_deck_set_param(r.a, FA_DECK_EQ_HIGH_DB, 6.0f), FA_OK);       /* boost: +6 dB */
    CHECK_NEAR(sine_rms(&r, 0, buf, 24576) / k_sine_rms, 1.995, 0.05);
    fa_deck_load(r.a, NULL, FA_DECK_LOAD_FORCE);
    fa_stream_close(s);
    fa_mixer_destroy(r.m);
    free(buf);
}

/* Crossfader curves, sides, trim, fader and master gain. */
static void test_gains(void)
{
    rig r;
    float* buf = (float*)malloc(sizeof(float) * 2 * 24576);
    float v = 0.0f;
    fa_stream* s;
    fa_xfader_curve curve;
    CHECK(buf && rig_open(&r, FA_CUE_NONE));
    s = load_sine(r.a, 1000.0);
    CHECK(s);
    CHECK_NEAR(sine_rms(&r, 0, buf, 24576) / k_sine_rms, 0.7071, 0.01);   /* middle, constant power */
    CHECK_EQ(fa_mixer_set_crossfader_curve(r.m, FA_XFADER_LINEAR), FA_OK);
    CHECK_EQ(fa_mixer_get_crossfader_curve(r.m, &curve), FA_OK);
    CHECK_EQ(curve, FA_XFADER_LINEAR);
    CHECK_NEAR(sine_rms(&r, 0, buf, 24576) / k_sine_rms, 0.5, 0.01);
    /* A curve change glides over 5 ms: the first millisecond stays near linear's 0.5
     * instead of stepping to constant power's 0.707. */
    CHECK_EQ(fa_mixer_set_crossfader_curve(r.m, FA_XFADER_CONSTANT_POWER), FA_OK);
    rig_render(&r, buf, 1024);
    CHECK(rms_of(buf, r.channels, g_rate / 1000, g_rate / 1000, 0) / k_sine_rms < 0.6);
    CHECK_NEAR(rms_of(buf, r.channels, 1024, 512, 0) / k_sine_rms, 0.7071, 0.02);
    CHECK_EQ(fa_mixer_set_crossfader_curve(r.m, FA_XFADER_LINEAR), FA_OK);
    fa_mixer_set_param(r.m, FA_MIX_CROSSFADER, 1.0f);                              /* all the way to B */
    CHECK(sine_rms(&r, 0, buf, 24576) / k_sine_rms < 0.001);
    CHECK_EQ(fa_deck_set_param(r.a, FA_DECK_XFADER_SIDE, 0.0f), FA_OK);             /* thru */
    CHECK_NEAR(sine_rms(&r, 0, buf, 24576) / k_sine_rms, 1.0, 0.01);
    CHECK_EQ(fa_deck_get_param(r.a, FA_DECK_XFADER_SIDE, &v), FA_OK);
    CHECK_EQ((int)v, 0);
    CHECK_EQ(fa_deck_set_param(r.a, FA_DECK_XFADER_SIDE, 1.5f), FA_ERROR_INVALID_ARG);
    CHECK_EQ(fa_deck_set_param(r.a, FA_DECK_TRIM_DB, 6.0f), FA_OK);
    CHECK_NEAR(sine_rms(&r, 0, buf, 24576) / k_sine_rms, 1.995, 0.03);
    CHECK_EQ(fa_deck_set_param(r.a, FA_DECK_GAIN, 0.5f), FA_OK);
    CHECK_NEAR(sine_rms(&r, 0, buf, 24576) / k_sine_rms, 0.998, 0.02);
    CHECK_EQ(fa_mixer_set_param(r.m, FA_MIX_MASTER_GAIN, 0.5f), FA_OK);
    CHECK_NEAR(sine_rms(&r, 0, buf, 24576) / k_sine_rms, 0.499, 0.01);
    CHECK_EQ(fa_mixer_get_param(r.m, FA_MIX_MASTER_GAIN, &v), FA_OK);
    CHECK_NEAR(v, 0.5, 1e-6);
    CHECK_EQ(fa_deck_ramp_param(r.a, FA_DECK_XFADER_SIDE, 1.0f, 0, 0, NULL), FA_ERROR_INVALID_ARG);
    fa_deck_load(r.a, NULL, FA_DECK_LOAD_FORCE);
    fa_stream_close(s);
    fa_mixer_destroy(r.m);
    free(buf);
}

/* A crossfader ramp runs on the device clock: half way through, the middle. */
static void test_crossfader_ramp(void)
{
    rig r;
    float* buf = (float*)malloc(sizeof(float) * 2 * 48000);
    float v = 0.0f;
    fa_stream* s;
    uint32_t done;
    CHECK(buf && rig_open(&r, FA_CUE_NONE));
    fa_mixer_set_param(r.m, FA_MIX_CROSSFADER, 0.0f);
    s = load_sine(r.a, 1000.0);
    CHECK(s);
    rig_render(&r, buf, 1024);
    CHECK_EQ(fa_mixer_ramp_param(r.m, FA_MIX_CROSSFADER, 1.0f, 1000000000, 0, NULL), FA_OK);
    for (done = 0; done < g_rate / 2; done += 512) rig_render(&r, buf, 512);
    CHECK_EQ(fa_mixer_get_param(r.m, FA_MIX_CROSSFADER, &v), FA_OK);
    CHECK_NEAR(v, 0.5, 0.02);
    for (done = 0; done < g_rate * 6 / 10; done += 512) rig_render(&r, buf, 512);
    CHECK_EQ(fa_mixer_get_param(r.m, FA_MIX_CROSSFADER, &v), FA_OK);
    CHECK_NEAR(v, 1.0, 1e-6);
    fa_deck_load(r.a, NULL, FA_DECK_LOAD_FORCE);
    fa_stream_close(s);
    fa_mixer_destroy(r.m);
    free(buf);
}

/* Channels 3-4: the cue is the pre-fader deck, and CUE_MIX blends in the master. */
static void test_cue_channels(void)
{
    rig r;
    float* buf = (float*)malloc(sizeof(float) * 4 * 24576);
    fa_stream* s;
    int32_t cue_ch = -1;
    CHECK(buf && rig_open(&r, FA_CUE_CHANNELS));
    CHECK_EQ(r.channels, 4);
    fa_mixer_get_output_channels(r.m, NULL, &cue_ch);
    CHECK_EQ(cue_ch, 2);
    s = load_sine(r.a, 1000.0);
    CHECK(s);
    fa_mixer_set_param(r.m, FA_MIX_CROSSFADER, 0.0f);
    fa_deck_set_param(r.a, FA_DECK_GAIN, 0.0f);                 /* fader down: silent in the room */
    CHECK_EQ(fa_deck_set_param(r.a, FA_DECK_CUE, 1.0f), FA_OK);
    sine_rms(&r, 0, buf, 24576);
    CHECK(rms_of(buf, 4, 24576, 12288, 0) < 1e-6);
    CHECK_NEAR(rms_of(buf, 4, 24576, 12288, 2) / k_sine_rms, 1.0, 0.01);   /* but in the headphones */
    CHECK_NEAR(rms_of(buf, 4, 24576, 12288, 3) / k_sine_rms, 1.0, 0.01);
    fa_device_set_mute(r.dev, 1);                               /* the device mute takes the cue too */
    sine_rms(&r, 0, buf, 24576);
    CHECK(rms_of(buf, 4, 24576, 12288, 2) < 1e-6);
    fa_device_set_mute(r.dev, 0);
    sine_rms(&r, 0, buf, 24576);
    CHECK_NEAR(rms_of(buf, 4, 24576, 12288, 2) / k_sine_rms, 1.0, 0.01);
    fa_deck_set_param(r.a, FA_DECK_CUE, 0.0f);
    fa_deck_set_param(r.a, FA_DECK_GAIN, 1.0f);
    fa_mixer_set_param(r.m, FA_MIX_CUE_MIX, 1.0f);              /* the master in the headphones */
    fa_mixer_set_param(r.m, FA_MIX_CUE_GAIN, 0.5f);
    sine_rms(&r, 0, buf, 24576);
    CHECK_NEAR(rms_of(buf, 4, 24576, 12288, 0) / k_sine_rms, 1.0, 0.01);
    CHECK_NEAR(rms_of(buf, 4, 24576, 12288, 2) / k_sine_rms, 0.5, 0.01);
    fa_deck_load(r.a, NULL, FA_DECK_LOAD_FORCE);
    fa_stream_close(s);
    fa_mixer_destroy(r.m);
    free(buf);
}

/* Split cue: the master in mono on the left, the cue in mono on the right. */
static void test_cue_split(void)
{
    rig r;
    float* buf = (float*)malloc(sizeof(float) * 2 * 24576);
    fa_stream* sa;
    fa_stream* sb;
    CHECK(buf && rig_open(&r, FA_CUE_SPLIT));
    CHECK_EQ(r.channels, 2);
    sa = load_sine(r.a, 1000.0);
    sb = load_sine(r.b, 1000.0);
    CHECK(sa && sb);
    fa_mixer_set_param(r.m, FA_MIX_CROSSFADER, 0.0f);   /* A in the room */
    fa_deck_set_param(r.b, FA_DECK_CUE, 1.0f);            /* B in the headphones */
    fa_deck_set_param(r.b, FA_DECK_GAIN, 0.5f);           /* the cue is before the fader */
    sine_rms(&r, 0, buf, 24576);
    CHECK_NEAR(rms_of(buf, 2, 24576, 12288, 0) / k_sine_rms, 1.0, 0.01);
    CHECK_NEAR(rms_of(buf, 2, 24576, 12288, 1) / k_sine_rms, 1.0, 0.01);
    CHECK_EQ(fa_deck_load(r.a, NULL, 0), FA_ERROR_BUSY);  /* A is playing and heard */
    CHECK_EQ(fa_deck_load(r.b, NULL, 0), FA_OK);           /* B is not heard in the room */
    CHECK_EQ(fa_deck_load(r.a, NULL, FA_DECK_LOAD_FORCE), FA_OK);
    fa_stream_close(sa);
    fa_stream_close(sb);
    fa_mixer_destroy(r.m);
    free(buf);
}

/* Deck meters: after the fader, before the crossfader. */
static void test_deck_meter(void)
{
    rig r;
    float* buf = (float*)malloc(sizeof(float) * 2 * 1024);
    float peak = 0.0f, rms = 0.0f;
    fa_stream* s;
    uint32_t done;
    CHECK(buf && rig_open(&r, FA_CUE_NONE));
    s = load_sine(r.a, 1000.0);
    CHECK(s);
    CHECK_EQ(fa_deck_get_levels(r.a, 50000000, &peak, &rms, 1), FA_OK);   /* starts the meter */
    fa_mixer_set_param(r.m, FA_MIX_CROSSFADER, 1.0f);                       /* closed on A: the meter still reads */
    fa_deck_set_param(r.a, FA_DECK_GAIN, 0.5f);
    for (done = 0; done < g_rate; done += 1024) rig_render(&r, buf, 1024);
    CHECK_EQ(fa_deck_get_levels(r.a, 50000000, &peak, &rms, 1), FA_OK);
    CHECK_NEAR(rms / k_sine_rms, 0.5, 0.02);
    CHECK_NEAR(peak, 0.125, 0.01);
    fa_deck_load(r.a, NULL, FA_DECK_LOAD_FORCE);
    fa_stream_close(s);
    fa_mixer_destroy(r.m);
    free(buf);
}

/* Unloading a deck whose loop has turned detaches before it clears the loop, so no
 * join is made (and thrown away): the only seek is the detach's, to where it was heard. */
static void test_unload_turned_loop(void)
{
    rig r;
    static mem_io m;
    float* buf = (float*)malloc(sizeof(float) * 8 * 1024);
    fa_stream* s = open_saw_io(&m, g_rate * 4);
    int i, last = -1;
    CHECK(buf && s && rig_open(&r, FA_CUE_NONE));
    CHECK_EQ(fa_deck_load(r.a, s, 0), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    CHECK_EQ(fa_deck_set_loop(r.a, 1000, 5000), FA_OK);
    for (i = 0; i < 8; i++) rig_render(&r, buf, 1024);   /* past the out point: turned */
    for (i = 0; i < 400 && fa_load32(&m.seeks) != last; i++) {   /* the decode-ahead settles */
        last = fa_load32(&m.seeks);
        fa_sleep_ms(20);
    }
    CHECK(s->jumped_back);
    fa_store32(&m.seeks, 0);
    CHECK_EQ(fa_deck_load(r.a, NULL, FA_DECK_LOAD_FORCE), FA_OK);
    CHECK(s->device == NULL);
    CHECK_EQ(fa_load32(&m.seeks), 1);
    fa_mixer_destroy(r.m);
    fa_stream_close(s);
    free(m.data);
    free(buf);
}

/* Loading moves a stream between decks; destroying the mixer leaves streams open;
 * closing the mixer's device destroys the mixer. Long callbacks run in blocks. */
static void test_decks_and_lifetime(void)
{
    rig r;
    float* buf = (float*)malloc(sizeof(float) * 2 * 5000);
    fa_stream* s = open_wav(g_rate, 1, 1000.0);
    fa_stream* got = NULL;
    fa_frames cue = -1, in = -1, out = -1;
    fa_stream_state st;
    CHECK(buf && s && rig_open(&r, FA_CUE_NONE));
    CHECK_EQ(fa_deck_load(r.a, s, 0), FA_OK);
    CHECK_EQ(fa_deck_load(r.b, s, 0), FA_OK);              /* moves to B */
    CHECK_EQ(fa_deck_get_stream(r.a, &got), FA_OK);
    CHECK(got == NULL);
    CHECK_EQ(fa_deck_get_stream(r.b, &got), FA_OK);
    CHECK(got == s);
    CHECK(fa_load_ptr(&s->bus) != NULL);
    CHECK_EQ(fa_deck_set_cue_point(r.b, 1000), FA_OK);
    CHECK_EQ(fa_stream_play(s), FA_OK);
    rig_render(&r, buf, 5000);                              /* more than a block */
    CHECK_EQ(fa_deck_cue(r.b), FA_OK);
    fa_stream_get_state(s, &st);
    CHECK_EQ(st, FA_STATE_PAUSED);
    CHECK_EQ(fa_deck_get_cue_point(r.b, &cue), FA_OK);
    CHECK_EQ(cue, 1000);
    CHECK_EQ(fa_deck_set_loop(r.b, 2000, 9000), FA_OK);
    CHECK_EQ(fa_deck_get_loop(r.b, &in, &out), FA_OK);
    CHECK_EQ(in, 2000);
    CHECK_EQ(out, 9000);
    CHECK_EQ(fa_deck_set_loop(r.a, 2000, 9000), FA_ERROR_NOT_FOUND);   /* empty deck */
    /* A stream that leaves its deck (moved elsewhere, or closed) leaves it empty. */
    CHECK_EQ(fa_stream_attach(s, g_dev, NULL), FA_OK);
    CHECK_EQ(fa_deck_get_stream(r.b, &got), FA_OK);
    CHECK(got == NULL);
    CHECK_EQ(fa_deck_load(r.b, s, 0), FA_OK);
    {
        fa_stream* s2 = open_wav(g_rate, 1, 500.0);
        CHECK(s2);
        CHECK_EQ(fa_deck_load(r.a, s2, 0), FA_OK);
        {
            fa_stream* s3 = open_wav(g_rate, 1, 500.0);   /* a deck holds one stream */
            CHECK(s3);
            CHECK_EQ(fa_crossfade(s2, s3, 1000000000LL, NULL), FA_ERROR_INVALID_ARG);
            CHECK_EQ(fa_stream_set_next(s2, s3, 0, NULL), FA_ERROR_INVALID_ARG);
            CHECK(s3->device == NULL);
            fa_stream_close(s3);
        }
        fa_stream_close(s2);
        CHECK_EQ(fa_deck_get_stream(r.a, &got), FA_OK);
        CHECK(got == NULL);
    }
    fa_stream_set_loop_region(s, 2000, 9000);
    fa_mixer_destroy(r.m);
    CHECK(s->device == NULL);                                   /* detached, still open */
    fa_stream_get_loop_region(s, &in, &out);
    CHECK_EQ(out, 0);
    CHECK(rig_open(&r, FA_CUE_NONE));
    CHECK_EQ(fa_deck_load(r.a, s, 0), FA_OK);
    fa_device_close(r.dev);                                     /* takes the mixer with it */
    CHECK(s->device == NULL);
    fa_stream_close(s);
    free(buf);
}

/* Two mixers loading each other's streams at once: every load takes the stream off the
 * other mixer's deck under that mixer's lock, so a deck never ends up holding a stream
 * another load detached, and the two locks cannot deadlock. After each load the test
 * looks at both decks under both locks (in the mixer's own order). */
typedef struct swapper {
    struct rig* mine;
    struct rig* other;
    fa_stream* first;
    fa_stream* second;
    int rounds;
    int errors, broken;
} swapper;

/* A deck's stream plays through that deck; a stream on a deck's bus is that deck's. */
static int deck_consistent(fa_deck* d)
{
    fa_stream* s = (fa_stream*)fa_load_ptr(&d->stream);
    return !s || ((fa_bus*)fa_load_ptr(&s->bus) == d->bus && s->device == d->mixer->device);
}

static int stream_consistent(fa_stream* s)
{
    fa_bus* bus = (fa_bus*)fa_load_ptr(&s->bus);
    return !bus || !bus->deck || fa_load_ptr(&bus->deck->stream) == s;
}

static int both_consistent(swapper* w)
{
    fa_mixer* lo = w->mine->m < w->other->m ? w->mine->m : w->other->m;
    fa_mixer* hi = lo == w->mine->m ? w->other->m : w->mine->m;
    int ok;
    fa_mutex_lock(&lo->lock);
    fa_mutex_lock(&hi->lock);
    ok = deck_consistent(w->mine->a) && deck_consistent(w->other->a) && stream_consistent(w->first) &&
         stream_consistent(w->second);
    fa_mutex_unlock(&hi->lock);
    fa_mutex_unlock(&lo->lock);
    return ok;
}

static void swap_proc(void* arg)
{
    swapper* w = (swapper*)arg;
    int i;
    for (i = 0; i < w->rounds; i++) {
        if (fa_deck_load(w->mine->a, w->first, FA_DECK_LOAD_FORCE) != FA_OK) w->errors++;
        if (!both_consistent(w)) w->broken++;
        if (fa_deck_load(w->mine->a, w->second, FA_DECK_LOAD_FORCE) != FA_OK) w->errors++;
        if (!both_consistent(w)) w->broken++;
    }
}

static void test_load_across_mixers(void)
{
    rig r1, r2;
    fa_stream* s1 = open_wav(g_rate * 4, 1, 1000.0);
    fa_stream* s2 = open_wav(g_rate * 4, 1, 500.0);
    swapper w1, w2;
    fa_thread t1, t2;
    CHECK(s1 && s2 && rig_open(&r1, FA_CUE_NONE) && rig_open(&r2, FA_CUE_NONE));
    CHECK_EQ(fa_deck_load(r1.a, s1, 0), FA_OK);
    CHECK_EQ(fa_deck_load(r2.a, s2, 0), FA_OK);
    memset(&w1, 0, sizeof(w1));
    memset(&w2, 0, sizeof(w2));
    w1.mine = &r1; w1.other = &r2; w1.first = s2; w1.second = s1;
    w2.mine = &r2; w2.other = &r1; w2.first = s1; w2.second = s2;
    w1.rounds = w2.rounds = 2000;
    CHECK_EQ(fa_thread_create(&t1, swap_proc, &w1), 0);
    CHECK_EQ(fa_thread_create(&t2, swap_proc, &w2), 0);
    fa_thread_join(&t1);
    fa_thread_join(&t2);
    CHECK_EQ(w1.errors + w2.errors, 0);
    CHECK_EQ(w1.broken + w2.broken, 0);
    CHECK(fa_load_ptr(&r1.a->stream) || fa_load_ptr(&r2.a->stream));   /* the last loads left a stream on a deck */
    fa_mixer_destroy(r1.m);
    fa_mixer_destroy(r2.m);
    fa_stream_close(s1);
    fa_stream_close(s2);
}

/* The second-device cue at its lowest latency never delays the master, so the master
 * has no delay line (taps keep no room for one either). */
static void test_lowest_latency_no_delay_line(void)
{
#if !(defined(__APPLE__) && TARGET_OS_IPHONE)
    fa_mixer_config c;
    fa_mixer* m = NULL;
    fa_device* dev = NULL;
    int lowest;
    for (lowest = 0; lowest < 2; lowest++) {
        fa_mixer_config_init(&c);
        c.cue_mode = FA_CUE_DEVICE;
        c.latency = FA_LATENCY_NORMAL;
        c.flags = lowest ? FA_MIXER_CUE_LOWEST_LATENCY : 0;
        CHECK_EQ(fa_mixer_create(g_engine, &c, &m), FA_OK);
        fa_mixer_get_device(m, &dev);
        CHECK_EQ(m->mdelay.buf == NULL, lowest);
        CHECK_EQ(dev->align_max == 0, lowest);
        fa_mixer_destroy(m);
    }
#endif
}

static void test_config_errors(void)
{
    fa_mixer_config c;
    fa_mixer* m = NULL;
    fa_mixer_config_init(&c);
    CHECK_EQ(c.latency, FA_LATENCY_LOW);
    c.deck_count = 4;
    CHECK_EQ(fa_mixer_create(g_engine, &c, &m), FA_ERROR_NOT_IMPLEMENTED);
    fa_mixer_config_init(&c);
    c.cue_mode = FA_CUE_DEVICE;
    c.cue_device_id = "zz";      /* not hex */
#if defined(__APPLE__) && TARGET_OS_IPHONE
    CHECK_EQ(fa_mixer_create(g_engine, &c, &m), FA_ERROR_NOT_IMPLEMENTED);   /* no second output on iOS */
#else
    CHECK_EQ(fa_mixer_create(g_engine, &c, &m), FA_ERROR_INVALID_ARG);
#endif
    fa_mixer_config_init(&c);
    c.flags = 2;
    CHECK_EQ(fa_mixer_create(g_engine, &c, &m), FA_ERROR_INVALID_ARG);
    CHECK(m == NULL);
    CHECK(strcmp(fa_result_string(FA_ERROR_NOT_ENOUGH_CHANNELS), "unknown error") != 0);
}

int main(void)
{
    fa_engine_config cfg;
    fa_engine_config_init(&cfg);
    cfg.flags = FA_ENGINE_NULL_OUTPUT;
    cfg.worker_threads = 2;
    if (fa_engine_create(&cfg, &g_engine) != FA_OK) { fprintf(stderr, "engine create failed\n"); return 1; }
    if (fa_device_open(g_engine, NULL, &g_dev) != FA_OK) { fprintf(stderr, "device open failed\n"); return 1; }
    g_rate = g_dev->rate;
    printf("null device at %u Hz\n", g_rate);

    RUN(test_loop_ahead_of_decoder);
    RUN(test_loop_out_inside_ring);
    RUN(test_loop_out_just_passed);
    RUN(test_loop_far_behind_goes_to_in);
    RUN(test_loop_off_carries_on);
    RUN(test_seek_past_loop_leaves_it);
    RUN(test_jump_moves_loop);
    RUN(test_reverse_loop_joins);
    RUN(test_reverse_loop_late_and_off);
    RUN(test_reverse_jump);
    RUN(test_turn_instant);
    RUN(test_turn_in_loop);
    RUN(test_turn_smooth);
    RUN(test_loop_with_tempo);
    RUN(test_failed_join_keeps_loop);
    RUN(test_loop_region_readers);
    RUN(test_isolator_flat);
    RUN(test_isolator_kills);
    RUN(test_gains);
    RUN(test_crossfader_ramp);
    RUN(test_cue_channels);
    RUN(test_cue_split);
    RUN(test_deck_meter);
    RUN(test_unload_turned_loop);
    RUN(test_decks_and_lifetime);
    RUN(test_load_across_mixers);
    RUN(test_lowest_latency_no_delay_line);
    RUN(test_config_errors);

    fa_device_close(g_dev);
    fa_engine_destroy(g_engine);
    if (g_failures) { fprintf(stderr, "%d failure(s)\n", g_failures); return 1; }
    printf("all mixer tests passed\n");
    return 0;
}
