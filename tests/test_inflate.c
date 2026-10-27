/* The DEFLATE decoder and ZIP extraction against zlib (test only; zlib is never
 * linked into the library). Streams of several kinds of data at every level and
 * strategy, with stored and fixed blocks mixed in by flushes, go through
 * fa_inflate, a ZIP in memory and the same ZIP read from a file, to memory and
 * to a temp file. Cut and changed streams must fail cleanly. Built only where
 * CMake finds zlib. */
#include "cdg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; return; } } while (0)
#define CHECK_EQ(a, b) do { long long va_ = (long long)(a), vb_ = (long long)(b); if (va_ != vb_) { \
    fprintf(stderr, "  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); g_failures++; return; } } while (0)
#define RUN(t) do { int before_ = g_failures; printf("%s\n", #t); t(); if (g_failures == before_) printf("  ok\n"); } while (0)

static uint32_t g_rng = 12345;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }

/* Kinds of data: noise, text-like, long runs, short-period repeats, skewed bytes
 * (long Huffman codes), and noise with repeats far back. */
static void make_data(uint8_t* p, size_t n, int kind)
{
    static const char words[] = "the quick brown fox jumps over a lazy dog while karaoke plays ";
    size_t i;
    for (i = 0; i < n; i++) {
        switch (kind) {
        case 0: p[i] = (uint8_t)rnd(); break;
        case 1: p[i] = (uint8_t)words[(i * 7 + (rnd() % 3 == 0 ? rnd() % 5 : 0)) % (sizeof(words) - 1)]; break;
        case 2: p[i] = (uint8_t)((i / 300) & 3); break;
        case 3: p[i] = (uint8_t)(i % (1 + (i / 4096) % 7)); break;
        case 4: { uint32_t r = rnd(); int b = 0; while ((r & 1) && b < 40) { r = r >> 1 | rnd() << 31; b++; } p[i] = (uint8_t)b; break; }
        default: p[i] = i >= 20000 && rnd() % 4 ? p[i - 20000 + rnd() % 3] : (uint8_t)rnd(); break;
        }
    }
}

/* Raw DEFLATE; with `flushes` the input goes in pieces with sync and full flushes
 * (empty stored blocks, and blocks that end mid-byte). */
static size_t deflate_raw(const uint8_t* in, size_t n, uint8_t* out, size_t cap, int level, int strategy,
                          int wbits, int flushes)
{
    z_stream z;
    size_t done = 0;
    memset(&z, 0, sizeof(z));
    if (deflateInit2(&z, level, Z_DEFLATED, -wbits, 1 + (int)(rnd() % 9), strategy) != Z_OK) return 0;
    z.next_out = out;
    z.avail_out = (uInt)cap;
    while (flushes && n - done > 1000) {
        size_t step = 1 + rnd() % 9000;
        if (step > n - done) step = n - done;
        z.next_in = (Bytef*)in + done;
        z.avail_in = (uInt)step;
        deflate(&z, rnd() % 2 ? Z_SYNC_FLUSH : Z_FULL_FLUSH);
        done += step;
    }
    z.next_in = (Bytef*)in + done;
    z.avail_in = (uInt)(n - done);
    deflate(&z, Z_FINISH);
    deflateEnd(&z);
    return z.total_out;
}

static void test_against_zlib(void)
{
    static const int strategies[] = { Z_DEFAULT_STRATEGY, Z_FILTERED, Z_HUFFMAN_ONLY, Z_RLE, Z_FIXED };
    size_t cap = 300000;
    uint8_t* data = (uint8_t*)malloc(cap);
    uint8_t* comp = (uint8_t*)malloc(cap * 2);
    uint8_t* out = (uint8_t*)malloc(cap + 1);
    int kind, level, s, runs = 0;
    CHECK(data && comp && out);
    for (kind = 0; kind < 6; kind++) {
        for (level = 0; level <= 9; level++) {
            for (s = 0; s < 5; s++) {
                size_t n = rnd() % 4 == 0 ? rnd() % 64 : rnd() % cap, cn, cut;
                int wbits = 9 + (int)(rnd() % 7);
                make_data(data, n, kind);
                cn = deflate_raw(data, n, comp, cap * 2, level, strategies[s], wbits, (int)(rnd() % 2));
                CHECK(cn > 0);
                memset(out, 0xA5, n + 1);
                CHECK_EQ(fa_inflate(comp, cn, out, n), FA_OK);
                CHECK(memcmp(out, data, n) == 0);
                CHECK_EQ(out[n], 0xA5);   /* nothing written past out_size (word copies overshoot) */
                /* The wrong size, a cut stream and a changed one never read or write out of bounds. */
                if (n) CHECK(fa_inflate(comp, cn, out, n - 1) != FA_OK);
                CHECK(fa_inflate(comp, cn, out, n + 1) != FA_OK);
                cut = cn > 1 ? rnd() % (cn - 1) : 0;
                CHECK(fa_inflate(comp, cut, out, n) != FA_OK);
                if (cn) {
                    comp[rnd() % cn] ^= (uint8_t)(1u << (rnd() % 8));
                    fa_inflate(comp, cn, out, n);
                }
                runs++;
            }
        }
    }
    printf("  %d streams\n", runs);
    free(data);
    free(comp);
    free(out);
}

static void put16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t* p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

/* One deflated entry as a whole archive. */
static size_t make_zip(uint8_t* z, const uint8_t* comp, size_t cn, const uint8_t* data, size_t n)
{
    size_t cd;
    uint32_t crc = (uint32_t)crc32(0, data, (uInt)n);
    memset(z, 0, 30);
    put32(z, 0x04034b50u); put16(z + 8, 8); put32(z + 14, crc); put32(z + 18, (uint32_t)cn);
    put32(z + 22, (uint32_t)n); put16(z + 26, 1); z[30] = 'a';
    memcpy(z + 31, comp, cn);
    cd = 31 + cn;
    memset(z + cd, 0, 47);
    put32(z + cd, 0x02014b50u); put16(z + cd + 10, 8); put32(z + cd + 16, crc); put32(z + cd + 20, (uint32_t)cn);
    put32(z + cd + 24, (uint32_t)n); put16(z + cd + 28, 1); z[cd + 46] = 'a';
    memset(z + cd + 47, 0, 22);
    put32(z + cd + 47, 0x06054b50u); put16(z + cd + 47 + 8, 1); put16(z + cd + 47 + 10, 1);
    put32(z + cd + 47 + 12, 47); put32(z + cd + 47 + 16, (uint32_t)cd);
    return cd + 47 + 22;
}

/* Entries larger than the 1 MB output buffer (the temp file path flushes and
 * keeps the window), read from memory and from a file (64 KB input pieces). */
static void test_zip_paths(void)
{
    size_t n = 3u << 20, cn, zn;
    uint8_t* data = (uint8_t*)malloc(n);
    uint8_t* comp = (uint8_t*)malloc(n + n / 8 + 1024);
    uint8_t* zip = (uint8_t*)malloc(n + n / 8 + 2048);
    const char* path = "inflate_test.zip";
    int kind;
    CHECK(data && comp && zip);
    for (kind = 0; kind < 6; kind++) {
        int from_file;
        make_data(data, n, kind);
        cn = deflate_raw(data, n, comp, n + n / 8 + 1024, 1 + kind, Z_DEFAULT_STRATEGY, 15, kind % 2);
        CHECK(cn > 0);
        zn = make_zip(zip, comp, cn, data, n);
        for (from_file = 0; from_file < 2; from_file++) {
            fa_source src;
            fa_zip_entry* e = NULL;
            uint32_t count = 0;
            uint8_t* got = NULL;
            FILE* f;
            long len;
            if (from_file) {
                FILE* w = fopen(path, "wb");
                CHECK(w && fwrite(zip, 1, zn, w) == zn);
                fclose(w);
                CHECK_EQ(fa_source_open_path(&src, path), FA_OK);
            } else {
                CHECK_EQ(fa_source_open_memory_ref(&src, zip, zn), FA_OK);
            }
            CHECK_EQ(fa_zip_list(&src, &e, &count), FA_OK);
            CHECK_EQ(count, 1);
            CHECK_EQ(fa_zip_extract(&src, e, (uint64_t)1 << 30, &got), FA_OK);
            CHECK(memcmp(got, data, n) == 0);
            free(got);
            f = tmpfile();
            CHECK(f);
            CHECK_EQ(fa_zip_extract_file(&src, e, f), FA_OK);
            len = ftell(f);
            CHECK_EQ(len, (long)n);
            got = (uint8_t*)malloc(n);
            rewind(f);
            CHECK(got && fread(got, 1, n, f) == n && memcmp(got, data, n) == 0);
            free(got);
            fclose(f);
            /* An entry claiming more than the temp-file limit (a hostile archive) is
             * refused before anything is written. */
            {
                fa_zip_entry big = *e;
                big.size = FA_ZIP_TEMP_LIMIT + 1;
                f = tmpfile();
                CHECK(f);
                CHECK_EQ(fa_zip_extract_file(&src, &big, f), FA_ERROR_TOO_LARGE);
                CHECK_EQ(ftell(f), 0);
                fclose(f);
            }
            /* A wrong CRC is caught on both paths. */
            e->crc32 ^= 1;
            got = NULL;
            CHECK(fa_zip_extract(&src, e, (uint64_t)1 << 30, &got) != FA_OK);
            f = tmpfile();
            CHECK(fa_zip_extract_file(&src, e, f) != FA_OK);
            fclose(f);
            free(e);
            fa_source_close(&src);
        }
        CHECK_EQ(fa_crc32(data, n), (uint32_t)crc32(0, data, (uInt)n));
    }
    remove(path);
    free(data);
    free(comp);
    free(zip);
}

int main(void)
{
    RUN(test_against_zlib);
    RUN(test_zip_paths);
    if (g_failures) { fprintf(stderr, "%d failure(s)\n", g_failures); return 1; }
    printf("all inflate tests passed\n");
    return 0;
}
