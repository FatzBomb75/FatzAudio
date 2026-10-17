/* Shared by fa_bench and bass_bench: a clock, peak memory, raw PCM output and the
 * one-line JSON result both tools print, so tools/bass-compare/run.py can treat
 * the two libraries exactly the same way. */
#ifndef BENCH_COMMON_H
#define BENCH_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#  include <psapi.h>
static double bench_now_ms(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)freq.QuadPart;
}
/* Peak working set of the process so far, in KB. */
static double bench_peak_kb(void)
{
    PROCESS_MEMORY_COUNTERS pmc;
    pmc.cb = sizeof(pmc);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return -1;
    return (double)pmc.PeakWorkingSetSize / 1024.0;
}
/* Current working set, in KB: the baseline before the first stream is opened. */
static double bench_current_kb(void)
{
    PROCESS_MEMORY_COUNTERS pmc;
    pmc.cb = sizeof(pmc);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return -1;
    return (double)pmc.WorkingSetSize / 1024.0;
}
#else
#  include <time.h>
static double bench_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}
/* Linux: VmHWM (peak) and VmRSS (current) from /proc/self/status, in KB; -1
 * elsewhere (macOS has no cheap equivalent of both, and memory is then left out). */
static double bench_status_kb(const char* key)
{
    char line[256];
    double v = -1;
    size_t n = strlen(key);
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return -1;
    while (fgets(line, sizeof(line), f))
        if (!strncmp(line, key, n)) { v = atof(line + n); break; }
    fclose(f);
    return v;
}
static double bench_peak_kb(void) { return bench_status_kb("VmHWM:"); }
static double bench_current_kb(void) { return bench_status_kb("VmRSS:"); }
#endif

/* Raw output: interleaved float32, no header (run.py knows rate and channels from
 * the JSON line). "-" discards the audio, for speed runs. */
static FILE* bench_open_out(const char* path)
{
    FILE* f;
    if (!strcmp(path, "-")) return NULL;
    f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    return f;
}

/* A number for the JSON line: JSON has no NaN or infinity, so those become null. */
static inline void bench_num(const char* key, double v)
{
    if (v != v || v > 1e300 || v < -1e300) printf("\"%s\":null", key);
    else printf("\"%s\":%.4f", key, v);
}

/* `extra` lists a tool's own commands (NULL for none). */
static void bench_usage(const char* tool, const char* lib_arg, const char* extra)
{
    fprintf(stderr,
            "usage: %s %sdecode <in> <out.f32|-> [--prescan]\n"
            "       %s %sinfo <in> [--prescan]\n"
            "       %s %sseek <in> <out.f32> [--prescan] <frames per seek> <frame> [<frame> ...]\n"
            "       %s %smulti <in> <streams>\n%s"
            "Prints one JSON line with the result; audio is raw interleaved float32.\n",
            tool, lib_arg, tool, lib_arg, tool, lib_arg, tool, lib_arg, extra ? extra : "");
}

#endif
