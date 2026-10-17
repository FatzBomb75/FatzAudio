/* bass_bench (Windows only): BASS's side of the comparison, the same commands and
 * JSON as fa_bench. BASS is a black box: bass.dll and its format add-ons are
 * loaded at run time from the folder given and only their documented public API
 * is called. Nothing of BASS is linked, shipped or inspected.
 *
 *   bass_bench <bass folder> decode <in> <out.f32|-> [--prescan]
 *   bass_bench <bass folder> info <in> [--prescan]
 *   bass_bench <bass folder> seek <in> <out.f32> [--prescan] <frames> <frame> ...
 *   bass_bench <bass folder> multi <in> <streams>
 *
 * Streams are opened the way an app decodes with BASS: BASS_StreamCreateFile with
 * BASS_STREAM_DECODE | BASS_SAMPLE_FLOAT (and BASS_STREAM_PRESCAN with --prescan),
 * on device 0 ("no sound"), with every add-on in the folder loaded through
 * BASS_PluginLoad, as an app does at startup. Positions are in bytes of float
 * audio (BASS_POS_BYTE), so frame * channels * 4. */
#include "bench_common.h"

/* From the BASS documentation (bass.h). */
#define BASS_SAMPLE_FLOAT 256
#define BASS_STREAM_PRESCAN 0x20000
#define BASS_STREAM_DECODE 0x200000
#define BASS_UNICODE 0x80000000u
#define BASS_POS_BYTE 0

typedef unsigned long long QWORD;
typedef struct {
    DWORD freq, chans, flags, ctype, origres;
    DWORD plugin, sample;
    const char* filename;
} BASS_CHANNELINFO;

static BOOL (WINAPI *BASS_Init)(int, DWORD, DWORD, HWND, const void*);
static BOOL (WINAPI *BASS_Free)(void);
static DWORD (WINAPI *BASS_GetVersion)(void);
static int (WINAPI *BASS_ErrorGetCode)(void);
static DWORD (WINAPI *BASS_PluginLoad)(const char*, DWORD);
static DWORD (WINAPI *BASS_StreamCreateFile)(BOOL, const void*, QWORD, QWORD, DWORD);
static BOOL (WINAPI *BASS_StreamFree)(DWORD);
static BOOL (WINAPI *BASS_ChannelGetInfo)(DWORD, BASS_CHANNELINFO*);
static QWORD (WINAPI *BASS_ChannelGetLength)(DWORD, DWORD);
static BOOL (WINAPI *BASS_ChannelSetPosition)(DWORD, QWORD, DWORD);
static DWORD (WINAPI *BASS_ChannelGetData)(DWORD, void*, DWORD);

#define CHUNK 4096

/* GetProcAddress through a generic function pointer (no cast warnings). */
typedef void (*any_fn)(void);
static any_fn need(HMODULE m, const char* name)
{
    any_fn f = (any_fn)GetProcAddress(m, name);
    if (!f) { fprintf(stderr, "bass.dll has no %s\n", name); exit(1); }
    return f;
}

/* Function pointers are filled through memcpy: C has no portable cast from the
 * generic pointer GetProcAddress returns. */
#define LOAD(name) do { any_fn f_ = need(m, #name); memcpy(&name, &f_, sizeof(f_)); } while (0)

static char plugins[512];

static void load_bass(const char* dir)
{
    static const char* addons[] = { "bass_aac.dll", "bass_ac3.dll", "bassflac.dll", "bassopus.dll", "basswma.dll" };
    char path[MAX_PATH * 2];
    HMODULE m;
    size_t i;
    snprintf(path, sizeof(path), "%s\\bass.dll", dir);
    m = LoadLibraryA(path);
    if (!m) { fprintf(stderr, "cannot load %s\n", path); exit(1); }
    LOAD(BASS_Init);
    LOAD(BASS_Free);
    LOAD(BASS_GetVersion);
    LOAD(BASS_ErrorGetCode);
    LOAD(BASS_PluginLoad);
    LOAD(BASS_StreamCreateFile);
    LOAD(BASS_StreamFree);
    LOAD(BASS_ChannelGetInfo);
    LOAD(BASS_ChannelGetLength);
    LOAD(BASS_ChannelSetPosition);
    LOAD(BASS_ChannelGetData);
    if (!BASS_Init(0, 48000, 0, NULL, NULL)) { fprintf(stderr, "BASS_Init: error %d\n", BASS_ErrorGetCode()); exit(1); }
    for (i = 0; i < sizeof(addons) / sizeof(addons[0]); i++) {
        snprintf(path, sizeof(path), "%s\\%s", dir, addons[i]);
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) continue;
        if (BASS_PluginLoad(path, 0)) {
            size_t len = strlen(plugins);
            snprintf(plugins + len, sizeof(plugins) - len, "%s%s", len ? "," : "", addons[i]);
        }
    }
}

static DWORD open_stream(const char* path, int prescan, double* open_ms, BASS_CHANNELINFO* info)
{
    wchar_t w[MAX_PATH * 2];
    DWORD h;
    double t0 = bench_now_ms();
    MultiByteToWideChar(CP_UTF8, 0, path, -1, w, MAX_PATH * 2);
    h = BASS_StreamCreateFile(FALSE, w, 0, 0, BASS_STREAM_DECODE | BASS_SAMPLE_FLOAT | BASS_UNICODE | (prescan ? BASS_STREAM_PRESCAN : 0));
    if (open_ms) *open_ms = bench_now_ms() - t0;
    if (!h) {
        printf("{\"lib\":\"bass\",\"error\":\"BASS error %d\"}\n", BASS_ErrorGetCode());
        exit(1);
    }
    BASS_ChannelGetInfo(h, info);
    return h;
}

static void print_info(DWORD h, const BASS_CHANNELINFO* info)
{
    QWORD len = BASS_ChannelGetLength(h, BASS_POS_BYTE);
    printf("\"lib\":\"bass\",\"version\":\"%08lx\",\"plugins\":\"%s\",\"codec\":\"ctype 0x%05lx\",\"rate\":%lu,\"channels\":%lu,\"length\":%lld,",
           (unsigned long)BASS_GetVersion(), plugins, (unsigned long)info->ctype, (unsigned long)info->freq,
           (unsigned long)info->chans, len == (QWORD)-1 ? -1LL : (long long)(len / (4 * info->chans)));
}

static int cmd_info(const char* in, int prescan)
{
    BASS_CHANNELINFO info;
    double open_ms;
    DWORD h = open_stream(in, prescan, &open_ms, &info);
    printf("{");
    print_info(h, &info);
    printf("\"open_ms\":%.3f}\n", open_ms);
    BASS_StreamFree(h);
    return 0;
}

/* Opens the file once and decodes its first second before memory is measured,
 * exactly as fa_bench does. */
static void warm_up(const char* path)
{
    BASS_CHANNELINFO info;
    DWORD h = open_stream(path, 0, NULL, &info);
    float* buf = (float*)malloc(sizeof(float) * CHUNK * info.chans);
    DWORD i;
    for (i = 0; i * CHUNK < info.freq; i++) {
        DWORD got = BASS_ChannelGetData(h, buf, (DWORD)(sizeof(float) * CHUNK * info.chans));
        if (got == (DWORD)-1 || got == 0) break;
    }
    free(buf);
    BASS_StreamFree(h);
}

static int cmd_decode(const char* in, const char* outp, int prescan)
{
    BASS_CHANNELINFO info;
    double base, t0, open_ms, first_ms = -1;
    DWORD h;
    FILE* out = bench_open_out(outp);
    float* buf;
    long long total = 0, reads = 0;
    warm_up(in);
    base = bench_mem_start();
    t0 = bench_now_ms();
    h = open_stream(in, prescan, &open_ms, &info);
    buf = (float*)malloc(sizeof(float) * CHUNK * info.chans);
    for (;;) {
        DWORD got = BASS_ChannelGetData(h, buf, (DWORD)(sizeof(float) * CHUNK * info.chans));
        if (got == (DWORD)-1 || got == 0) break;
        if (first_ms < 0) first_ms = bench_now_ms() - t0;
        if (out) fwrite(buf, 1, got, out);
        if (reads++ % 16 == 0) bench_mem_sample();
        total += got / (4 * info.chans);
    }
    printf("{");
    print_info(h, &info);
    printf("\"frames\":%lld,\"open_ms\":%.3f,\"first_ms\":%.3f,\"total_ms\":%.3f,\"base_kb\":%.0f,\"peak_kb\":%.0f,\"result\":\"BASS error %d\"}\n",
           total, open_ms, first_ms, bench_now_ms() - t0, base, bench_peak_kb(), BASS_ErrorGetCode());
    if (out) fclose(out);
    free(buf);
    BASS_StreamFree(h);
    return 0;
}

static int cmd_seek(const char* in, const char* outp, int prescan, long long frames, char** pos, int npos)
{
    BASS_CHANNELINFO info;
    DWORD h = open_stream(in, prescan, NULL, &info);
    FILE* out = bench_open_out(outp);
    size_t fsize = 4 * info.chans;
    char* buf = (char*)calloc((size_t)frames, fsize);
    double worst = 0;
    int i;
    printf("{");
    print_info(h, &info);
    printf("\"seeks\":[");
    for (i = 0; i < npos; i++) {
        long long target = atoll(pos[i]), have = 0;
        double t0 = bench_now_ms(), ms;
        BOOL ok = BASS_ChannelSetPosition(h, (QWORD)target * fsize, BASS_POS_BYTE);
        while (ok && have < frames) {
            DWORD got = BASS_ChannelGetData(h, buf + have * fsize, (DWORD)((frames - have) * fsize));
            if (got == (DWORD)-1 || got == 0) break;
            have += got / fsize;
        }
        ms = bench_now_ms() - t0;
        if (ms > worst) worst = ms;
        memset(buf + have * fsize, 0, (size_t)(frames - have) * fsize);
        if (out) fwrite(buf, fsize, (size_t)frames, out);
        printf("%s{\"frame\":%lld,\"ok\":%d,\"got\":%lld,\"ms\":%.3f}", i ? "," : "", target, ok ? 1 : 0, have, ms);
    }
    printf("],\"worst_ms\":%.3f}\n", worst);
    if (out) fclose(out);
    free(buf);
    BASS_StreamFree(h);
    return 0;
}

static int cmd_multi(const char* in, int n)
{
    DWORD* h = (DWORD*)calloc((size_t)n, sizeof(DWORD));
    int* done = (int*)calloc((size_t)n, sizeof(int));
    BASS_CHANNELINFO info;
    double base, t0, open_ms = 0;
    float* buf;
    long long total = 0;
    int i, left = n;
    warm_up(in);
    base = bench_mem_start();
    t0 = bench_now_ms();
    for (i = 0; i < n; i++) {
        double ms;
        h[i] = open_stream(in, 0, &ms, &info);
        open_ms += ms;
    }
    buf = (float*)malloc(sizeof(float) * CHUNK * info.chans);
    while (left > 0) {
        bench_mem_sample();
        for (i = 0; i < n; i++) {
            DWORD got;
            if (done[i]) continue;
            got = BASS_ChannelGetData(h[i], buf, (DWORD)(sizeof(float) * CHUNK * info.chans));
            if (got == (DWORD)-1 || got == 0) { done[i] = 1; left--; continue; }
            total += got / (4 * info.chans);
        }
    }
    printf("{");
    print_info(h[0], &info);
    printf("\"streams\":%d,\"frames\":%lld,\"open_ms\":%.3f,\"total_ms\":%.3f,\"base_kb\":%.0f,\"peak_kb\":%.0f}\n",
           n, total, open_ms, bench_now_ms() - t0, base, bench_peak_kb());
    for (i = 0; i < n; i++) BASS_StreamFree(h[i]);
    free(buf);
    free(done);
    free(h);
    return 0;
}

int main(int argc, char** argv)
{
    int prescan = 0, rc, i;
    const char* cmd;
    if (argc < 4) { bench_usage(argv[0], "<bass folder> ", NULL); return 2; }
    for (i = 2; i < argc; i++)
        if (!strcmp(argv[i], "--prescan")) {
            prescan = 1;
            memmove(argv + i, argv + i + 1, sizeof(char*) * (size_t)(argc - i - 1));
            argc--;
            i--;
        }
    load_bass(argv[1]);
    cmd = argv[2];
    if (!strcmp(cmd, "info")) rc = cmd_info(argv[3], prescan);
    else if (!strcmp(cmd, "decode") && argc > 4) rc = cmd_decode(argv[3], argv[4], prescan);
    else if (!strcmp(cmd, "seek") && argc > 6) rc = cmd_seek(argv[3], argv[4], prescan, atoll(argv[5]), argv + 6, argc - 6);
    else if (!strcmp(cmd, "multi") && argc > 4) rc = cmd_multi(argv[3], atoi(argv[4]));
    else { bench_usage(argv[0], "<bass folder> ", NULL); rc = 2; }
    BASS_Free();
    return rc;
}
