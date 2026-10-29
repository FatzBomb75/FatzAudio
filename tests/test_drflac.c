/* The patched dr_flac (third_party/miniaudio/PATCHES.md) on its own: cases the
 * library's seek chain can't reach, as it starts over from the first frame
 * whenever a search fails, but another caller of dr_flac (or a later change to
 * the chain) could. This file compiles its own static copy of miniaudio's
 * decoders. */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#define MA_API static
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DEVICE_IO
#define MA_NO_ENGINE
#define MA_NO_NODE_GRAPH
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_GENERATION
#define MA_NO_ENCODING
#define MA_NO_WAV
#define MA_NO_MP3
#define MA_NO_THREADING
#if defined(__GNUC__)
#  pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "miniaudio.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; return; } } while (0)
#define CHECK_EQ(a, b) do { long long va_ = (long long)(a), vb_ = (long long)(b); if (va_ != vb_) { \
    fprintf(stderr, "  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); g_failures++; return; } } while (0)
#define RUN(t) do { int before_ = g_failures; printf("%s\n", #t); t(); if (g_failures == before_) printf("  ok\n"); } while (0)

/* A .flac cut off right after frame 1, whose STREAMINFO still counts 8 frames. A seek past
 * the cut fails after reading on from frame 1 for the next header (dr_flac's frame-by-frame
 * search, with the bisection switched off as dr_flac's own tests do); a seek back inside
 * frame 1 must then decode it again, not take the samples left from before. */
static uint8_t flac_crc8(const uint8_t* d, size_t n)
{
    uint8_t c = 0;
    size_t i;
    int k;
    for (i = 0; i < n; i++) { c ^= d[i]; for (k = 0; k < 8; k++) c = (uint8_t)((c & 0x80) ? (c << 1) ^ 0x07 : c << 1); }
    return c;
}

static uint16_t flac_crc16(const uint8_t* d, size_t n)
{
    uint16_t c = 0;
    size_t i;
    int k;
    for (i = 0; i < n; i++) { c ^= (uint16_t)(d[i] << 8); for (k = 0; k < 8; k++) c = (uint16_t)((c & 0x8000) ? (c << 1) ^ 0x8005 : c << 1); }
    return c;
}

static int16_t cut_sample(uint32_t i) { return (int16_t)((int32_t)(i * 7u % 30011u) - 15005); }

static void test_failed_seek_drops_old_frame(void)
{
    enum { BLOCK = 1024, FRAMES = 2, COUNTED = 8 };
    uint8_t d[42 + FRAMES * (8 + 1 + 2 * BLOCK + 2)];
    size_t n = 0, h;
    uint32_t f, i;
    ma_dr_flac* fl;
    int16_t out[64];
    static const uint8_t head[42] = { 'f', 'L', 'a', 'C', 0x80, 0, 0, 34,
                                      BLOCK >> 8, BLOCK & 255, BLOCK >> 8, BLOCK & 255, 0, 0, 0, 0, 0, 0,
                                      0x0A, 0xC4, 0x40, 0xF0, 0, 0, COUNTED * BLOCK >> 8, COUNTED * BLOCK & 255 };
    memcpy(d, head, sizeof(head));           /* 44.1 kHz mono 16-bit; MD5 left zero */
    n = sizeof(head);
    for (f = 0; f < FRAMES; f++) {
        h = n;
        d[n++] = 0xFF; d[n++] = 0xF8;        /* fixed block size */
        d[n++] = 0x70;                       /* block size: 16 bits follow; rate from STREAMINFO */
        d[n++] = 0x08;                       /* mono, 16 bits */
        d[n++] = (uint8_t)f;                 /* frame number */
        d[n++] = (BLOCK - 1) >> 8; d[n++] = (BLOCK - 1) & 255;
        d[n] = flac_crc8(d + h, n - h); n++;
        d[n++] = 0x02;                       /* VERBATIM */
        for (i = 0; i < BLOCK; i++) {
            uint16_t v = (uint16_t)cut_sample(f * BLOCK + i);
            d[n++] = (uint8_t)(v >> 8); d[n++] = (uint8_t)v;
        }
        { uint16_t c = flac_crc16(d + h, n - h); d[n++] = (uint8_t)(c >> 8); d[n++] = (uint8_t)c; }
    }
    fl = ma_dr_flac_open_memory(d, n, NULL);
    CHECK(fl != NULL);
    fl->_noBinarySearchSeek = MA_TRUE;
    CHECK_EQ(fl->totalPCMFrameCount, COUNTED * BLOCK);
    CHECK_EQ(ma_dr_flac_read_pcm_frames_s16(fl, BLOCK + 300, NULL), BLOCK + 300);
    CHECK(!ma_dr_flac_seek_to_pcm_frame(fl, 5 * BLOCK));
    CHECK(ma_dr_flac_seek_to_pcm_frame(fl, BLOCK + 176));
    CHECK_EQ(ma_dr_flac_read_pcm_frames_s16(fl, 64, out), 64);
    for (i = 0; i < 64; i++) {
        if (out[i] != cut_sample(BLOCK + 176 + i)) {
            fprintf(stderr, "  frame %u is %d, expected %d\n", BLOCK + 176 + i, out[i], cut_sample(BLOCK + 176 + i));
            ma_dr_flac_close(fl);
            CHECK(0);
        }
    }
    ma_dr_flac_close(fl);
}

int main(void)
{
    RUN(test_failed_seek_drops_old_frame);
    printf(g_failures ? "FAILED (%d)\n" : "PASS\n", g_failures);
    return g_failures ? 1 : 0;
}
