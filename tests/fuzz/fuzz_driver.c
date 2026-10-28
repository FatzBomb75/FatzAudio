/* A small coverage-guided fuzz driver for the LLVMFuzzerTestOneInput targets in this
 * directory, for compilers without libFuzzer (GCC: -fsanitize-coverage=trace-pc).
 * It keeps every input that reaches a new edge, mutates the kept inputs, and writes
 * the input that crashed or hung to the output directory before it dies.
 *
 *   fuzz_x [-n execs] [-t seconds] [-s seed] [-m maxlen] [-o outdir] corpus_dir_or_file...
 *
 * Inputs that gave new coverage are saved to outdir/queue (reusable as seeds). With
 * libFuzzer (clang -fsanitize=fuzzer) the same targets build without this file. */
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

#define MAP_BITS 18
static uint8_t g_virgin[1u << MAP_BITS];   /* edges seen so far */
static uint8_t g_hit[1u << MAP_BITS];      /* edges of the current run */
static int g_new;

void __sanitizer_cov_trace_pc(void)
{
    uintptr_t a = (uintptr_t)__builtin_return_address(0);
    uint32_t h = (uint32_t)((a >> 2) * 2654435761u) >> (32 - MAP_BITS);
    g_hit[h] = 1;
}

typedef struct { uint8_t* p; size_t n; } item;
static item* g_q; static size_t g_qn, g_qcap;
static uint64_t g_rng = 88172645463325252ull;
static const char* g_out = "fuzz-out";
static const uint8_t* g_cur; static size_t g_cur_n;
static uint64_t g_execs;

static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return (uint32_t)(g_rng >> 11); }
static uint32_t below(uint32_t n) { return n ? rnd() % n : 0; }

static void mkdir_p(const char* dir)
{
    char tmp[600], *p;
    snprintf(tmp, sizeof(tmp), "%s", dir);
    for (p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(tmp, 0777); *p = '/'; }
    mkdir(tmp, 0777);
}

static void save(const char* dir, const char* prefix, const uint8_t* p, size_t n)
{
    char path[600];
    FILE* f;
    uint32_t h = 2166136261u;
    size_t i;
    for (i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    mkdir_p(g_out);
    snprintf(path, sizeof(path), "%s/%s", g_out, dir);
    mkdir(path, 0777);
    snprintf(path, sizeof(path), "%s/%s/%s-%08x", g_out, dir, prefix, h);
    f = fopen(path, "wb");
    if (f) { fwrite(p, 1, n, f); fclose(f); fprintf(stderr, "saved %s (%zu bytes)\n", path, n); }
    else fprintf(stderr, "cannot save %s\n", path);
}

static void on_fatal(int sig)
{
    /* Only for the dying process: not async-signal-safe, which does not matter here. */
    fprintf(stderr, "\nsignal %d after %llu execs\n", sig, (unsigned long long)g_execs);
    save("crashes", sig == SIGALRM ? "hang" : "crash", g_cur, g_cur_n);
    _exit(77);
}

static void add(const uint8_t* p, size_t n, int persist)
{
    if (g_qn == g_qcap) { g_qcap = g_qcap ? g_qcap * 2 : 256; g_q = (item*)realloc(g_q, g_qcap * sizeof(item)); }
    g_q[g_qn].p = (uint8_t*)malloc(n ? n : 1);
    memcpy(g_q[g_qn].p, p, n);
    g_q[g_qn].n = n;
    g_qn++;
    if (persist) save("queue", "q", p, n);
}

static int run(const uint8_t* p, size_t n)
{
    size_t i;
    memset(g_hit, 0, sizeof(g_hit));
    g_cur = p; g_cur_n = n;
    alarm(20);
    LLVMFuzzerTestOneInput(p, n);
    alarm(0);
    g_execs++;
    g_new = 0;
    for (i = 0; i < sizeof(g_hit); i++)
        if (g_hit[i] && !g_virgin[i]) { g_virgin[i] = 1; g_new++; }
    return g_new;
}

static const uint8_t k8[] = { 0, 1, 2, 3, 4, 7, 8, 15, 16, 31, 32, 63, 64, 127, 128, 129, 254, 255 };
static const uint32_t k32[] = { 0, 1, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF, 0x8000, 0xFFFF, 0x10000, 0x7FFFFFFF, 0x80000000u, 0xFFFFFFFFu,
                                0x00FFFFFF, 0x01000000, 0x40000000 };

static size_t mutate(uint8_t* b, size_t n, size_t cap)
{
    int rounds = 1 + (int)below(4), r;
    for (r = 0; r < rounds; r++) {
        switch (below(11)) {
        case 0: if (n) b[below((uint32_t)n)] ^= (uint8_t)(1u << below(8)); break;
        case 1: if (n) b[below((uint32_t)n)] = (uint8_t)rnd(); break;
        case 2: if (n) b[below((uint32_t)n)] = k8[below(sizeof(k8))]; break;
        case 3: if (n >= 4) { uint32_t v = k32[below(sizeof(k32) / 4)], at = below((uint32_t)n - 3); if (rnd() & 1) v = __builtin_bswap32(v); memcpy(b + at, &v, 4); } break;
        case 4: if (n >= 2) { uint16_t v = (uint16_t)k32[below(sizeof(k32) / 4)], at = (uint16_t)0; at = (uint16_t)below((uint32_t)n - 1); memcpy(b + at, &v, 2); } break;
        case 5: if (n > 1) { size_t len = 1 + below((uint32_t)(n < 64 ? n : 64)), at = below((uint32_t)(n - len + 1)); memmove(b + at, b + at + len, n - at - len); n -= len; } break;
        case 6: if (n && n < cap) { size_t len = 1 + below((uint32_t)(n < 64 ? n : 64)), at = below((uint32_t)n);
                if (n + len > cap) len = cap - n;
                memmove(b + at + len, b + at, n - at);
                memset(b + at, (int)rnd(), len); n += len; } break;
        case 7: if (n > 2) { size_t len = 1 + below((uint32_t)(n < 128 ? n / 2 : 64)), from = below((uint32_t)(n - len + 1)), at = below((uint32_t)(n - len + 1)); memmove(b + at, b + from, len); } break;
        case 8: if (n > 1) n = 1 + below((uint32_t)n); break;   /* cut short */
        case 9: if (n) { size_t at = below((uint32_t)n); b[at] = (uint8_t)(b[at] + (below(7) - 3)); } break;
        default: if (n) { size_t at = below((uint32_t)n), len = 1 + below(16); size_t i; for (i = 0; i < len && at + i < n; i++) b[at + i] = (uint8_t)rnd(); } break;
        }
    }
    return n;
}

static void load_path(const char* path)
{
    struct stat st;
    if (stat(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        DIR* d = opendir(path);
        struct dirent* e;
        while (d && (e = readdir(d)) != NULL) {
            char sub[1024];
            if (e->d_name[0] == '.') continue;
            snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
            load_path(sub);
        }
        if (d) closedir(d);
    } else if (S_ISREG(st.st_mode) && st.st_size >= 0 && st.st_size < (8 << 20)) {
        FILE* f = fopen(path, "rb");
        uint8_t* p = (uint8_t*)malloc((size_t)st.st_size + 1);
        if (f && fread(p, 1, (size_t)st.st_size, f) == (size_t)st.st_size) { run(p, (size_t)st.st_size); add(p, (size_t)st.st_size, 0); }
        if (f) fclose(f);
        free(p);
    }
}

int main(int argc, char** argv)
{
    uint64_t max_execs = 1000000, last = 0;
    double max_s = 1e9;
    size_t maxlen = 1 << 20, i;
    int a;
    time_t t0 = time(NULL), tl = t0;
    uint8_t* buf;
    signal(SIGALRM, on_fatal); signal(SIGABRT, on_fatal); signal(SIGILL, on_fatal);   /* sanitizer reports: abort(), or a trap for UBSan; a bare SEGV is reported by ASan first */
    for (a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "-n") && a + 1 < argc) max_execs = strtoull(argv[++a], NULL, 10);
        else if (!strcmp(argv[a], "-t") && a + 1 < argc) max_s = atof(argv[++a]);
        else if (!strcmp(argv[a], "-s") && a + 1 < argc) g_rng ^= strtoull(argv[++a], NULL, 10) * 0x9E3779B97F4A7C15ull;
        else if (!strcmp(argv[a], "-m") && a + 1 < argc) maxlen = (size_t)strtoull(argv[++a], NULL, 10);
        else if (!strcmp(argv[a], "-o") && a + 1 < argc) g_out = argv[++a];
        else load_path(argv[a]);
    }
    if (!g_qn) { fprintf(stderr, "no seeds\n"); return 2; }
    fprintf(stderr, "%zu seeds, %llu execs of seeds, %d edges\n", g_qn, (unsigned long long)g_execs, 0);
    buf = (uint8_t*)malloc(maxlen + 64);
    while (g_execs < max_execs && difftime(time(NULL), t0) < max_s) {
        item* s = &g_q[below((uint32_t)g_qn)];
        size_t n = s->n < maxlen ? s->n : maxlen;
        memcpy(buf, s->p, n);
        n = mutate(buf, n, maxlen);
        if (run(buf, n)) add(buf, n, 1);
        if (time(NULL) - tl >= 30) {
            size_t edges = 0;
            for (i = 0; i < sizeof(g_virgin); i++) edges += g_virgin[i];
            fprintf(stderr, "%llu execs (%llu/s), queue %zu, edges %zu\n", (unsigned long long)g_execs,
                    (unsigned long long)((g_execs - last) / 30), g_qn, edges);
            last = g_execs; tl = time(NULL);
        }
    }
    {
        size_t edges = 0;
        for (i = 0; i < sizeof(g_virgin); i++) edges += g_virgin[i];
        fprintf(stderr, "DONE %llu execs in %.0f s, queue %zu, edges %zu, no crash\n", (unsigned long long)g_execs,
                difftime(time(NULL), t0), g_qn, edges);
    }
    for (i = 0; i < g_qn; i++) free(g_q[i].p);
    free(g_q);
    free(buf);
    return 0;
}
