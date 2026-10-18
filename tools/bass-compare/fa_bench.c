/* fa_bench: FatzAudio's side of the BASS comparison (tools/bass-compare/README.md).
 * Uses only the public API, on decode-only streams, and prints one JSON line.
 * It uses no FFmpeg.
 *
 *   fa_bench decode <in> <out.f32|-> [--prescan]
 *   fa_bench info <in> [--prescan]
 *   fa_bench seek <in> <out.f32> [--prescan] <frames> <frame> ...
 *   fa_bench multi <in> <streams>
 *   fa_bench loudness <in>
 *   fa_bench disc <image.cue> <track> <out.f32|->
 *
 * decode   the whole file: reported length, decoded frames, time to open, time to
 *          the first audio, total time, memory.
 * info     opens the file and prints its format and reported length.
 * seek     seeks to each frame and reads <frames> frames there, all into one file.
 * multi    opens <streams> streams of the same file and decodes them all, in turn,
 *          to the end: throughput and memory with many streams.
 * loudness fa_scan_loudness_path (integrated loudness, range, true peak).
 * disc     decodes one track of a disc image, like decode.
 *
 * The play commands play through the default output device instead, while
 * loop_rec records it (README.md, "Playback on a real device"). They print the
 * time of each call on loop_rec's clock, so run.py can line the calls up with what
 * was recorded:
 * play-latency    plays <click.wav> <trials> times, <gap ms> apart (start to start).
 * play-crossfade  plays <a.wav>, then after <start after ms> crossfades to <b.wav>
 *                 over <fade ms> with fa_crossfade (linear, or equal power).
 * play-gapless    queues <b.wav> after <a.wav> with fa_stream_set_next (no
 *                 crossfade), as a player does for gapless albums. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L  /* clock_gettime */
#endif
#include "fatzaudio/fatzaudio.h"
#include "bench_common.h"

#define CHUNK 4096
#define FA_USAGE "       fa_bench loudness <in>\n" \
                 "       fa_bench disc <image.cue> <track> <out.f32|->\n"

static fa_engine* engine;

static fa_stream* open_stream(const char* path, int prescan, double* open_ms)
{
    fa_open_options opts;
    fa_stream* s = NULL;
    fa_result r;
    double t0 = bench_now_ms();
    fa_open_options_init(&opts);
    opts.flags = FA_OPEN_DECODE_ONLY | FA_OPEN_NO_CDG | (prescan ? FA_OPEN_PRESCAN : 0);
    r = fa_stream_open_path(engine, path, &opts, &s);
    if (open_ms) *open_ms = bench_now_ms() - t0;
    if (r != FA_OK) {
        printf("{\"lib\":\"fatzaudio\",\"error\":\"%s\"}\n", fa_result_string(r));
        exit(1);
    }
    return s;
}

static void print_info(fa_stream* s)
{
    fa_stream_info info;
    fa_stream_get_info(s, &info);
    printf("\"lib\":\"fatzaudio\",\"codec\":\"%s\",\"rate\":%u,\"channels\":%u,\"length\":%lld,",
           info.codec, info.format.sample_rate, info.format.channels, (long long)info.length_frames);
}

/* Opens the file once and decodes its first second, so code pages, worker threads
 * and other one-time setup are in place before memory is measured: what is left
 * is what a stream itself costs. bass_bench does the same. */
static void warm_up(const char* path)
{
    fa_stream_info info;
    fa_stream* s = open_stream(path, 0, NULL);
    float* buf;
    int i;
    fa_stream_get_info(s, &info);
    buf = (float*)malloc(sizeof(float) * CHUNK * info.format.channels);
    for (i = 0; i * CHUNK < (int)info.format.sample_rate; i++) {
        fa_frames got = 0;
        if (fa_stream_read(s, buf, CHUNK, &got) != FA_OK || got == 0) break;
    }
    free(buf);
    fa_stream_close(s);
}

/* Reads the stream to the end and prints the decode result. */
static int decode_all(fa_stream* s, const char* outp, double t0, double open_ms, double base)
{
    fa_stream_info info;
    double first_ms = -1;
    FILE* out = bench_open_out(outp);
    float* buf;
    long long total = 0, reads = 0;
    fa_result r;
    fa_stream_get_info(s, &info);
    buf = (float*)malloc(sizeof(float) * CHUNK * info.format.channels);
    for (;;) {
        fa_frames got = 0;
        r = fa_stream_read(s, buf, CHUNK, &got);
        if (got > 0 && first_ms < 0) first_ms = bench_now_ms() - t0;
        if (got > 0 && out) fwrite(buf, sizeof(float) * info.format.channels, (size_t)got, out);
        if (reads++ % 16 == 0) bench_mem_sample();
        total += got;
        if (r != FA_OK || got == 0) break;
    }
    printf("{");
    print_info(s);
    printf("\"frames\":%lld,\"open_ms\":%.3f,\"first_ms\":%.3f,\"total_ms\":%.3f,\"base_kb\":%.0f,\"peak_kb\":%.0f,\"result\":\"%s\"}\n",
           total, open_ms, first_ms, bench_now_ms() - t0, base, bench_peak_kb(), fa_result_string(r));
    if (out) fclose(out);
    free(buf);
    fa_stream_close(s);
    return 0;
}

static int cmd_decode(const char* in, const char* outp, int prescan)
{
    double base, t0, open_ms;
    fa_stream* s;
    warm_up(in);
    base = bench_mem_start();
    t0 = bench_now_ms();
    s = open_stream(in, prescan, &open_ms);
    return decode_all(s, outp, t0, open_ms, base);
}

static int cmd_info(const char* in, int prescan)
{
    double open_ms;
    fa_stream* s = open_stream(in, prescan, &open_ms);
    printf("{");
    print_info(s);
    printf("\"open_ms\":%.3f}\n", open_ms);
    fa_stream_close(s);
    return 0;
}

/* A track of a .cue image (or a drive), through fa_disc_open + fa_stream_open_disc. */
static int cmd_disc(const char* cue, int track, const char* outp)
{
    double base = bench_mem_start(), t0 = bench_now_ms();
    fa_open_options opts;
    fa_disc* disc = NULL;
    fa_stream* s = NULL;
    fa_result r;
    int rc;
    if ((r = fa_disc_open(cue, NULL, &disc)) != FA_OK) {
        printf("{\"lib\":\"fatzaudio\",\"error\":\"%s\"}\n", fa_result_string(r));
        return 1;
    }
    fa_open_options_init(&opts);
    opts.flags = FA_OPEN_DECODE_ONLY;
    if ((r = fa_stream_open_disc(engine, disc, track, &opts, &s)) != FA_OK) {
        printf("{\"lib\":\"fatzaudio\",\"error\":\"%s\"}\n", fa_result_string(r));
        fa_disc_close(disc);
        return 1;
    }
    rc = decode_all(s, outp, t0, bench_now_ms() - t0, base);
    fa_disc_close(disc);
    return rc;
}

static int cmd_seek(const char* in, const char* outp, int prescan, long long frames, char** pos, int npos)
{
    fa_stream_info info;
    fa_stream* s = open_stream(in, prescan, NULL);
    FILE* out = bench_open_out(outp);
    float* buf;
    double t0, worst = 0;
    int i;
    fa_stream_get_info(s, &info);
    buf = (float*)calloc((size_t)frames * info.format.channels, sizeof(float));
    printf("{");
    print_info(s);
    printf("\"seeks\":[");
    for (i = 0; i < npos; i++) {
        long long target = atoll(pos[i]), have = 0;
        fa_result r;
        double ms;
        t0 = bench_now_ms();
        r = fa_stream_seek(s, (fa_frames)target);
        while (r == FA_OK && have < frames) {
            fa_frames got = 0;
            if (fa_stream_read(s, buf + have * info.format.channels, (fa_frames)(frames - have), &got) != FA_OK || got == 0) break;
            have += got;
        }
        ms = bench_now_ms() - t0;
        if (ms > worst) worst = ms;
        /* Short reads (seek failed, or past the end) are padded with silence so every
         * block in the file has the same size. */
        memset(buf + have * info.format.channels, 0, sizeof(float) * (size_t)(frames - have) * info.format.channels);
        if (out) fwrite(buf, sizeof(float) * info.format.channels, (size_t)frames, out);
        printf("%s{\"frame\":%lld,\"ok\":%d,\"got\":%lld,\"ms\":%.3f}", i ? "," : "", target, r == FA_OK, have, ms);
    }
    printf("],\"worst_ms\":%.3f}\n", worst);
    if (out) fclose(out);
    free(buf);
    fa_stream_close(s);
    return 0;
}

static int cmd_multi(const char* in, int n)
{
    fa_stream** s = (fa_stream**)calloc((size_t)n, sizeof(*s));
    int* done = (int*)calloc((size_t)n, sizeof(int));
    double base, t0, open_ms = 0;
    fa_stream_info info;
    float* buf;
    long long total = 0;
    int i, left = n;
    warm_up(in);
    base = bench_mem_start();
    t0 = bench_now_ms();
    for (i = 0; i < n; i++) {
        double ms;
        s[i] = open_stream(in, 0, &ms);
        open_ms += ms;
    }
    fa_stream_get_info(s[0], &info);
    buf = (float*)malloc(sizeof(float) * CHUNK * info.format.channels);
    while (left > 0) {
        bench_mem_sample();
        for (i = 0; i < n; i++) {
            fa_frames got = 0;
            if (done[i]) continue;
            if (fa_stream_read(s[i], buf, CHUNK, &got) != FA_OK || got == 0) { done[i] = 1; left--; }
            total += got;
        }
    }
    printf("{");
    print_info(s[0]);
    printf("\"streams\":%d,\"frames\":%lld,\"open_ms\":%.3f,\"total_ms\":%.3f,\"base_kb\":%.0f,\"peak_kb\":%.0f}\n",
           n, total, open_ms, bench_now_ms() - t0, base, bench_peak_kb());
    for (i = 0; i < n; i++) fa_stream_close(s[i]);
    free(buf);
    free(done);
    free((void*)s);
    return 0;
}

static int cmd_loudness(const char* in)
{
    fa_loudness l;
    double t0 = bench_now_ms();
    fa_result r = fa_scan_loudness_path(engine, in, &l);
    if (r != FA_OK) { printf("{\"lib\":\"fatzaudio\",\"error\":\"%s\"}\n", fa_result_string(r)); return 1; }
    printf("{\"lib\":\"fatzaudio\",");
    bench_num("lufs", l.integrated_lufs);
    printf(",");
    bench_num("lra", l.range_lu);
    printf(",");
    bench_num("true_peak", l.true_peak_dbtp);
    printf(",\"total_ms\":%.3f,\"peak_kb\":%.0f}\n", bench_now_ms() - t0, bench_process_peak_kb());
    return 0;
}

/* ---- Playback on the default device (the play commands) ----------------- */

static fa_device* device;

static fa_stream* open_playing(const char* path)
{
    fa_open_options opts;
    fa_stream* s = NULL;
    fa_result r;
    fa_open_options_init(&opts);
    opts.flags = FA_OPEN_NO_CDG;
    r = fa_stream_open_path(engine, path, &opts, &s);
    if (r == FA_OK) r = fa_stream_attach(s, device, NULL);
    if (r != FA_OK) {
        printf("{\"lib\":\"fatzaudio\",\"error\":\"%s: %s\"}\n", path, fa_result_string(r));
        exit(1);
    }
    return s;
}

static void open_device(void)
{
    fa_result r = fa_device_open(engine, NULL, &device);
    if (r == FA_OK) r = fa_device_start(device);
    if (r != FA_OK) {
        printf("{\"lib\":\"fatzaudio\",\"error\":\"device: %s\"}\n", fa_result_string(r));
        exit(1);
    }
}

static void print_device(void)
{
    fa_time lat = 0;
    fa_device_get_latency(device, &lat);
    printf("\"lib\":\"fatzaudio\",\"reported_latency_ms\":%.3f,", (double)lat / 1e6);
}

static double length_ms(fa_stream* s)
{
    fa_stream_info info;
    fa_stream_get_info(s, &info);
    return info.length_frames > 0 ? (double)info.length_frames * 1000.0 / info.format.sample_rate : 0;
}

/* Waits until the stream has ended, or at most `max_ms`. */
static void wait_ended(fa_stream* s, double max_ms)
{
    double t_end = bench_now_ms() + max_ms;
    fa_stream_state st = FA_STATE_PLAYING;
    while (bench_now_ms() < t_end && fa_stream_get_state(s, &st) == FA_OK && st != FA_STATE_ENDED)
        bench_sleep_ms(5);
}

static int cmd_play_latency(const char* click, int trials, double gap_ms)
{
    double* t = (double*)calloc((size_t)trials, sizeof(double));
    fa_stream* s;
    int i;
    open_device();
    s = open_playing(click);
    bench_sleep_ms(300);  /* let the device settle */
    for (i = 0; i < trials; i++) {
        double t0 = bench_now_ms();
        t[i] = t0;
        fa_stream_play(s);
        bench_sleep_ms(gap_ms - (bench_now_ms() - t0) - 20);
        fa_stream_stop(s);  /* rewinds for the next trial */
        bench_sleep_ms(gap_ms - (bench_now_ms() - t0));
    }
    printf("{");
    print_device();
    bench_print_times("calls", t, trials);
    printf("}\n");
    fa_stream_close(s);
    fa_device_close(device);
    free(t);
    return 0;
}

static int cmd_play_crossfade(const char* a, const char* b, double fade_ms, double after_ms, int equal)
{
    fa_ramp ramp;
    fa_stream* sa;
    fa_stream* sb;
    double t_play, t_fade;
    fa_result r;
    open_device();
    sa = open_playing(a);
    sb = open_playing(b);
    memset(&ramp, 0, sizeof(ramp));
    ramp.curve = equal ? FA_CURVE_EQUAL_POWER : FA_CURVE_LINEAR;
    bench_sleep_ms(300);
    t_play = bench_now_ms();
    fa_stream_play(sa);
    bench_sleep_ms(after_ms);
    t_fade = bench_now_ms();
    r = fa_crossfade(sa, sb, (fa_time)(fade_ms * 1e6), &ramp);
    bench_sleep_ms(fade_ms + 1000);
    printf("{");
    print_device();
    printf("\"curve\":\"%s\",\"play\":%.3f,\"fade\":%.3f,\"result\":\"%s\"}\n", equal ? "equal" : "linear",
           t_play, t_fade, fa_result_string(r));
    fa_stream_close(sa);
    fa_stream_close(sb);
    fa_device_close(device);
    return 0;
}

static int cmd_play_gapless(const char* a, const char* b)
{
    fa_stream* sa;
    fa_stream* sb;
    double t_play;
    fa_result r;
    open_device();
    sa = open_playing(a);
    sb = open_playing(b);
    r = fa_stream_set_next(sa, sb, 0, NULL);
    bench_sleep_ms(300);
    t_play = bench_now_ms();
    fa_stream_play(sa);
    wait_ended(sb, length_ms(sa) + length_ms(sb) + 3000);
    bench_sleep_ms(300);
    printf("{");
    print_device();
    printf("\"play\":%.3f,\"result\":\"%s\"}\n", t_play, fa_result_string(r));
    fa_stream_close(sa);
    fa_stream_close(sb);
    fa_device_close(device);
    return 0;
}

int main(int argc, char** argv)
{
    fa_engine_config cfg;
    int a = 1, prescan = 0, rc, i;
    const char* cmd;
    if (a + 1 >= argc) { bench_usage(argv[0], "", FA_USAGE); return 2; }
    cmd = argv[a];
    for (i = a; i < argc; i++)
        if (!strcmp(argv[i], "--prescan")) {
            prescan = 1;
            memmove(argv + i, argv + i + 1, sizeof(char*) * (size_t)(argc - i - 1));
            argc--;
            i--;
        }
    fa_engine_config_init(&cfg);
    if (strncmp(cmd, "play-", 5)) cfg.flags = FA_ENGINE_NULL_OUTPUT;
    if (fa_engine_create(&cfg, &engine) != FA_OK) { fprintf(stderr, "engine failed\n"); return 1; }
    if (!strcmp(cmd, "info")) rc = cmd_info(argv[a + 1], prescan);
    else if (!strcmp(cmd, "decode") && a + 2 < argc) rc = cmd_decode(argv[a + 1], argv[a + 2], prescan);
    else if (!strcmp(cmd, "seek") && a + 4 < argc) rc = cmd_seek(argv[a + 1], argv[a + 2], prescan, atoll(argv[a + 3]), argv + a + 4, argc - a - 4);
    else if (!strcmp(cmd, "multi") && a + 2 < argc) rc = cmd_multi(argv[a + 1], atoi(argv[a + 2]));
    else if (!strcmp(cmd, "disc") && a + 3 < argc) rc = cmd_disc(argv[a + 1], atoi(argv[a + 2]), argv[a + 3]);
    else if (!strcmp(cmd, "loudness")) rc = cmd_loudness(argv[a + 1]);
    else if (!strcmp(cmd, "play-latency") && a + 3 < argc) rc = cmd_play_latency(argv[a + 1], atoi(argv[a + 2]), atof(argv[a + 3]));
    else if (!strcmp(cmd, "play-crossfade") && a + 4 < argc)
        rc = cmd_play_crossfade(argv[a + 1], argv[a + 2], atof(argv[a + 3]), atof(argv[a + 4]), a + 5 < argc && !strcmp(argv[a + 5], "equal"));
    else if (!strcmp(cmd, "play-gapless") && a + 2 < argc) rc = cmd_play_gapless(argv[a + 1], argv[a + 2]);
    else { bench_usage(argv[0], "", FA_USAGE); rc = 2; }
    fa_engine_destroy(engine);
    return rc;
}
