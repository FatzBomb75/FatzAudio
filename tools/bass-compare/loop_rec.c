/* loop_rec: records what reaches the sound card, for the playback measurements in
 * tools/bass-compare (README.md, "Playback on a real device"). It belongs to
 * neither library: it opens its own capture device through miniaudio, so FatzAudio
 * and BASS are recorded by exactly the same code.
 *
 *   loop_rec <out.f32> <seconds> [--capture <name>] [--list]
 *
 * Records for <seconds> at most; it stops sooner once a file <out.f32>.stop
 * exists (run.py creates it when the player is done).
 * Default (Windows): WASAPI loopback of the default output device, i.e. the mix
 * the system sends to the speakers. --capture records an input device whose name
 * contains <name> instead: a loopback cable, "Stereo Mix", or a PulseAudio monitor
 * ("Monitor of ...") on Linux. --list prints the capture devices.
 *
 * Writes 48 kHz stereo float32, and prints one JSON line with the time stamps of
 * every capture callback (frames recorded so far, monotonic ms on the same clock
 * as fa_bench and bass_bench), from which run.py works out when each frame was
 * captured. Before recording it prints {"ready":...} so run.py knows when to start
 * the player. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L  /* clock_gettime */
#endif
#include "miniaudio.h"  /* compiled in loop_rec_miniaudio.c */
#include "bench_common.h"

#define RATE 48000
#define CHANNELS 2
#define MAX_STAMPS 200000

typedef struct rec {
    float* buf;
    ma_uint64 cap;
    ma_uint64 frames;      /* written by the audio thread, polled by main: under lock */
    ma_spinlock lock;
    double* stamp_ms;
    ma_uint64* stamp_frames;
    ma_uint32 stamps;
} rec;

static void on_data(ma_device* dev, void* out, const void* in, ma_uint32 frames)
{
    rec* r = (rec*)dev->pUserData;
    double now = bench_now_ms();
    ma_uint64 have, n = frames;
    ma_spinlock_lock(&r->lock);
    have = r->frames;
    ma_spinlock_unlock(&r->lock);
    (void)out;
    if (have + n > r->cap) n = r->cap - have;
    if (n) memcpy(r->buf + have * CHANNELS, in, (size_t)n * CHANNELS * sizeof(float));
    if (r->stamps < MAX_STAMPS) {
        r->stamp_ms[r->stamps] = now;
        r->stamp_frames[r->stamps] = have + n;  /* frames recorded once this callback is done */
        r->stamps++;
    }
    ma_spinlock_lock(&r->lock);
    r->frames = have + n;
    ma_spinlock_unlock(&r->lock);
}

static ma_uint64 frames_now(rec* r)
{
    ma_uint64 n;
    ma_spinlock_lock(&r->lock);
    n = r->frames;
    ma_spinlock_unlock(&r->lock);
    return n;
}

/* A JSON string: device names are the system's and may hold quotes or backslashes. */
static void print_json_string(const char* v)
{
    putchar('"');
    for (; *v; v++) {
        if (*v == '"' || *v == '\\') printf("\\%c", *v);
        else if ((unsigned char)*v < 0x20) printf("\\u%04x", (unsigned char)*v);
        else putchar(*v);
    }
    putchar('"');
}

static int stop_requested(const char* stop_path)
{
    FILE* f = fopen(stop_path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static int find_capture(ma_context* ctx, const char* name, ma_device_id* out_id, char* out_name, int list)
{
    ma_device_info* caps;
    ma_uint32 n, i;
    if (ma_context_get_devices(ctx, NULL, NULL, &caps, &n) != MA_SUCCESS) return 0;
    for (i = 0; i < n; i++) {
        if (list) printf("%s%s\n", caps[i].name, caps[i].isDefault ? "  (default)" : "");
        else if (strstr(caps[i].name, name)) {
            *out_id = caps[i].id;
            snprintf(out_name, 256, "%s", caps[i].name);
            return 1;
        }
    }
    return 0;
}

int main(int argc, char** argv)
{
    ma_context ctx;
    ma_device_config cfg;
    ma_device dev;
    ma_device_id id;
    char devname[256] = "default output (loopback)";
    const char* capture = NULL;
    double seconds, t_end;
    FILE* out;
    rec r;
    ma_uint32 i;
    char stop_path[1024];
    int a, list = 0, written = 0;
    for (a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "--capture") && a + 1 < argc) capture = argv[++a];
        else if (!strcmp(argv[a], "--list")) list = 1;
    }
    if (ma_context_init(NULL, 0, NULL, &ctx) != MA_SUCCESS) { fprintf(stderr, "no audio backend\n"); return 1; }
    if (list) { find_capture(&ctx, "", &id, devname, 1); return 0; }
    if (argc < 3) {
        fprintf(stderr, "usage: %s <out.f32> <seconds> [--capture <name>] [--list]\n", argv[0]);
        return 2;
    }
    seconds = atof(argv[2]);
    snprintf(stop_path, sizeof(stop_path), "%s.stop", argv[1]);
    remove(stop_path);
    memset(&r, 0, sizeof(r));
    r.cap = (ma_uint64)(seconds * RATE) + RATE;
    r.buf = (float*)calloc((size_t)r.cap * CHANNELS, sizeof(float));
    r.stamp_ms = (double*)calloc(MAX_STAMPS, sizeof(double));
    r.stamp_frames = (ma_uint64*)calloc(MAX_STAMPS, sizeof(ma_uint64));
    if (capture) {
        if (!find_capture(&ctx, capture, &id, devname, 0)) {
            printf("{\"error\":\"no capture device named like that (try --list)\"}\n");
            return 1;
        }
        cfg = ma_device_config_init(ma_device_type_capture);
        cfg.capture.pDeviceID = &id;
    } else {
        cfg = ma_device_config_init(ma_device_type_loopback);  /* WASAPI only */
    }
    cfg.capture.format = ma_format_f32;
    cfg.capture.channels = CHANNELS;
    cfg.sampleRate = RATE;
    cfg.periodSizeInMilliseconds = 5;  /* small periods: finer time stamps */
    cfg.dataCallback = on_data;
    cfg.pUserData = &r;
    if (ma_device_init(&ctx, &cfg, &dev) != MA_SUCCESS || ma_device_start(&dev) != MA_SUCCESS) {
        printf("{\"error\":\"cannot open the capture device\",\"device\":");
        print_json_string(devname);
        printf("}\n");
        return 1;
    }
    /* Ready once audio arrives: some backends take a while to deliver the first period. */
    t_end = bench_now_ms() + 5000.0;
    while (frames_now(&r) == 0 && bench_now_ms() < t_end) bench_sleep_ms(1);
    printf("{\"ready\":%.3f}\n", bench_now_ms());
    fflush(stdout);
    t_end = bench_now_ms() + seconds * 1000.0;
    while (bench_now_ms() < t_end && frames_now(&r) < r.cap - RATE && !stop_requested(stop_path))
        bench_sleep_ms(10);
    ma_device_uninit(&dev);
    ma_context_uninit(&ctx);
    out = bench_open_out(argv[1]);  /* exits with a message if it cannot */
    if (out) {
        written = fwrite(r.buf, sizeof(float) * CHANNELS, (size_t)r.frames, out) == (size_t)r.frames;
        written = (fclose(out) == 0) && written;
    }
    printf("{\"device\":");
    print_json_string(devname);
    printf(",\"written\":%d,\"rate\":%d,\"channels\":%d,\"frames\":%llu,\"stamps\":[", written, RATE, CHANNELS,
           (unsigned long long)r.frames);
    for (i = 0; i < r.stamps; i++)
        printf("%s[%llu,%.3f]", i ? "," : "", (unsigned long long)r.stamp_frames[i], r.stamp_ms[i]);
    printf("]}\n");
    fflush(stdout);  /* everything is out even if the process is slow to exit */
    return 0;
}
