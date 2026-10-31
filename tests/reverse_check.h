/* Reverse playback checks shared by the decoder tests: a decode-only stream read
 * whole, forward or reversed, and how the reversed audio compares with the straight
 * decode last frame first, at the joins between reverse_read's blocks above all.
 * Include after fa_internal.h. */
#ifndef FA_TEST_REVERSE_CHECK_H
#define FA_TEST_REVERSE_CHECK_H

#include <math.h>
#include <stdlib.h>

/* Reads `s` (decode-only) to its end: forward, or reversed from frame `from` (>= 0).
 * Closes it. NULL if it would not seek or reverse. */
static float* rc_read_all(fa_stream* s, int64_t from, int64_t* out_n, uint32_t* out_ch, uint32_t* out_rate)
{
    fa_stream_info info;
    float* buf;
    int64_t n = 0, cap = 1 << 15;
    uint32_t ch;
    fa_stream_get_info(s, &info);
    ch = info.format.channels;
    *out_rate = info.format.sample_rate;
    if (from >= 0 && (fa_stream_seek(s, from) != FA_OK || fa_stream_set_reverse(s, 1) != FA_OK)) {
        fa_stream_close(s);
        return NULL;
    }
    buf = (float*)malloc((size_t)cap * ch * sizeof(float));
    for (;;) {
        fa_frames got = 0;
        fa_result r;
        if (n + 1000 > cap) {
            cap *= 2;
            buf = (float*)realloc(buf, (size_t)cap * ch * sizeof(float));
        }
        r = fa_stream_read(s, buf + (size_t)n * ch, 1000, &got);
        n += got;
        if (r != FA_OK || got == 0) break;
    }
    fa_stream_close(s);
    *out_n = n;
    *out_ch = ch;
    return buf;
}

/* `rev` (n frames, read reversed from n) against `full` (the straight decode):
 * the largest difference from it, and the most that a step between neighbouring
 * frames at a block join exceeds the largest step around it (in either decode,
 * within 600 frames). Blocks start at n - block - ov, n - 2 * block - ov, ...: the
 * joins are the ov frames below n - block, n - 2 * block, ... */
static void rc_compare(const float* full, const float* rev, int64_t n, uint32_t ch, uint32_t rate, double* out_worst,
                       double* out_excess)
{
    int64_t block = (int64_t)rate * FA_REVERSE_BLOCK_MS / 1000, ov = (int64_t)rate * FA_REVERSE_OVERLAP_MS / 1000, i, j, q;
    double worst = 0, excess = 0;
    uint32_t c;
    for (i = 0; i < n * (int64_t)ch; i++) {
        double e = fabs((double)rev[i] - full[(size_t)(n - 1 - i / ch) * ch + (size_t)(i % ch)]);
        if (e > worst) worst = e;
    }
    for (j = n - block; j > ov + 700; j -= block)
        for (c = 0; c < ch; c++) {
            double at_join = 0, around = 0;
            for (q = j - ov - 600; q <= j + 600 && q < n; q++) {   /* source frames q - 1 and q */
                double u = fabs((double)full[(size_t)q * ch + c] - full[(size_t)(q - 1) * ch + c]);
                double t = fabs((double)rev[(size_t)(n - 1 - q) * ch + c] - rev[(size_t)(n - q) * ch + c]);
                if (q >= j - ov - 2 && q <= j + 2) { if (t > at_join) at_join = t; }
                else if (t > around) around = t;
                if (u > around) around = u;
            }
            if (at_join - around > excess) excess = at_join - around;
        }
    *out_worst = worst;
    *out_excess = excess;
}

#endif
