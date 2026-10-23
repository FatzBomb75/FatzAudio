/* fa_mic_check: runs the DJ mixer's setup check (fa_mixer_measure_latency) on a real
 * output and mic and prints what it measured, for trying a rig without an app.
 *
 *   fa_mic_check --list
 *   fa_mic_check [--mic NAME] [--out NAME] [--mode normal|low|exclusive] [--all]
 *                [--level DB] [--repeat N] [--null]
 *
 * NAME matches part of a device name (case-sensitive); without it the system default is
 * used. --all measures every latency mode, --level is the loudest test sound in dBFS
 * (-40..-6, default -12), --repeat runs the check N times, --null runs it on the silent null output (a smoke test:
 * nothing is heard). */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L   /* nanosleep */
#endif
#include <fatzaudio/fatzaudio.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#define sleep_ms(ms) Sleep(ms)
#else
#include <time.h>
static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
#endif

static const char* mode_name(fa_latency_mode m)
{
    return m == FA_LATENCY_NORMAL ? "normal" : m == FA_LATENCY_LOW ? "low" : "exclusive";
}

static const char* advice_name(fa_check_advice a)
{
    switch (a) {
    case FA_CHECK_MONITOR: return "monitor";
    case FA_CHECK_SLOW: return "slow (monitor off)";
    case FA_CHECK_FEEDBACK: return "feedback (monitor off, move the mic)";
    default: return "not heard";
    }
}

/* The id of the first device of `type` whose name contains `part`; 0 when none. */
static int find_device(fa_engine* e, fa_device_type type, const char* part, fa_device_info* out)
{
    uint32_t n = 0, i;
    fa_device_count(e, type, &n);
    for (i = 0; i < n; i++)
        if (fa_device_get_info(e, type, i, out) == FA_OK && strstr(out->name, part)) return 1;
    return 0;
}

static void list(fa_engine* e, fa_device_type type, const char* title)
{
    uint32_t n = 0, i;
    fa_device_info info;
    fa_device_count(e, type, &n);
    printf("%s:\n", title);
    for (i = 0; i < n; i++)
        if (fa_device_get_info(e, type, i, &info) == FA_OK) printf("  %s\n", info.name);
}

int main(int argc, char** argv)
{
    const char *mic_name = NULL, *out_name = NULL;
    fa_latency_mode mode = FA_LATENCY_LOW;
    int all = 0, do_list = 0, null_out = 0, repeat = 1, i;
    float level = -12.0f;
    fa_engine_config ec;
    fa_engine* engine = NULL;
    fa_mixer_config mxc;
    fa_mixer* mixer = NULL;
    fa_mic_config mc;
    fa_mic* mic = NULL;
    fa_device_info out_info, mic_info;
    fa_result r;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--list")) do_list = 1;
        else if (!strcmp(argv[i], "--all")) all = 1;
        else if (!strcmp(argv[i], "--null")) null_out = 1;
        else if (!strcmp(argv[i], "--mic") && i + 1 < argc) mic_name = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_name = argv[++i];
        else if (!strcmp(argv[i], "--level") && i + 1 < argc) level = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--repeat") && i + 1 < argc) repeat = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
            const char* m = argv[++i];
            mode = !strcmp(m, "normal") ? FA_LATENCY_NORMAL : !strcmp(m, "exclusive") ? FA_LATENCY_EXCLUSIVE : FA_LATENCY_LOW;
        } else {
            fprintf(stderr, "usage: fa_mic_check [--list] [--mic NAME] [--out NAME] [--mode normal|low|exclusive] [--all] [--level DB] [--repeat N] [--null]\n");
            return 2;
        }
    }

    fa_engine_config_init(&ec);
    if (null_out) ec.flags |= FA_ENGINE_NULL_OUTPUT;
    r = fa_engine_create(&ec, &engine);
    if (r != FA_OK) {
        fprintf(stderr, "engine: %s\n", fa_result_string(r));
        return 1;
    }
    if (do_list) {
        list(engine, FA_DEVICE_PLAYBACK, "outputs");
        list(engine, FA_DEVICE_CAPTURE, "inputs");
        fa_engine_destroy(engine);
        return 0;
    }

    fa_mixer_config_init(&mxc);
    mxc.latency = mode;
    if (out_name) {
        if (!find_device(engine, FA_DEVICE_PLAYBACK, out_name, &out_info)) {
            fprintf(stderr, "no output named like \"%s\" (--list)\n", out_name);
            fa_engine_destroy(engine);
            return 1;
        }
        mxc.device_id = out_info.id;
    }
    r = fa_mixer_create(engine, &mxc, &mixer);
    if (r != FA_OK) {
        fprintf(stderr, "mixer (%s): %s\n", mode_name(mode), fa_result_string(r));
        fa_engine_destroy(engine);
        return 1;
    }
    fa_mic_config_init(&mc);
    mc.monitor = 0;   /* the check silences the mics anyway; keep the room quiet between runs */
    if (mic_name) {
        if (!find_device(engine, FA_DEVICE_CAPTURE, mic_name, &mic_info)) {
            fprintf(stderr, "no input named like \"%s\" (--list)\n", mic_name);
            fa_mixer_destroy(mixer);
            fa_engine_destroy(engine);
            return 1;
        }
        mc.device_id = mic_info.id;
    }
    r = fa_mixer_add_mic(mixer, &mc, &mic);
    if (r != FA_OK) {
        fprintf(stderr, "mic: %s\n", fa_result_string(r));
        fa_mixer_destroy(mixer);
        fa_engine_destroy(engine);
        return 1;
    }
    sleep_ms(500);   /* let the input and the bridge settle */

    for (i = 0; i < repeat; i++) {
        fa_latency_check_config cc;
        fa_latency_check res;
        fa_mixer_latency lat;
        uint32_t k;
        fa_latency_check_config_init(&cc);
        cc.flags = all ? FA_CHECK_ALL_MODES : 0;
        cc.max_level_db = level;
        memset(&res, 0, sizeof(res));
        res.struct_size = sizeof(res);
        r = fa_mixer_measure_latency(mixer, mic, &cc, &res);
        printf("run %d: %s, advice %s in %s\n", i + 1, fa_result_string(r), advice_name(res.advice),
               mode_name(res.advised_mode));
        for (k = 0; k < res.count; k++) {
            const fa_latency_check_mode* m = &res.modes[k];
            if (m->result != FA_OK) {
                printf("  %-9s %s\n", mode_name(m->mode), fa_result_string(m->result));
                continue;
            }
            printf("  %-9s mic->speaker %6.2f ms (spread %.2f ms, reported %6.2f ms), max gain %+5.1f dB, level %+5.1f dBFS\n",
                   mode_name(m->mode), m->mic_to_speaker / 1e6, m->spread / 1e6, m->reported / 1e6,
                   m->max_gain_db, m->level_db);
        }
        memset(&lat, 0, sizeof(lat));
        lat.struct_size = sizeof(lat);
        if (fa_mixer_get_latency(mixer, &lat) == FA_OK)
            printf("  now: master %.2f ms, mic input %.2f ms, mic monitor %.2f ms\n", lat.master / 1e6,
                   lat.mic_input[0] / 1e6, lat.mic_monitor[0] / 1e6);
        fflush(stdout);
    }

    fa_mic_remove(mic);
    fa_mixer_destroy(mixer);
    fa_engine_destroy(engine);
    return r == FA_OK ? 0 : 1;
}
