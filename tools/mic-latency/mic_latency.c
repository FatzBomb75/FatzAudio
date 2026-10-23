/* Mic latency spike (DJ mixer plan, step 0).
 *
 * Measures how long a sound takes from a microphone to the speakers on this machine, through the
 * path FatzAudio's mixer will use: a capture device, a ring buffer, a playback device. Two tests per
 * latency mode:
 *
 *   round trip   a short chirp is played every 0.5 s with monitoring off; the time from writing it to
 *                the output to finding it in the microphone is output + input latency (+ the air
 *                between speaker and mic, about 3 ms per metre). From callback timestamps.
 *   monitor loop the microphone is played back through the speakers (gain kept below feedback) and
 *                one chirp is injected; the distance between its repeats in the recording is the
 *                real mic-to-speaker latency of the monitoring path (+ the air). Sample-exact.
 *
 * Modes: normal (shared, the default 10 ms period: what FatzAudio does today), low (shared, smallest
 * period the driver offers through IAudioClient3, native rate), exclusive (WASAPI exclusive, 3 ms,
 * for reference only).
 *
 * Usage: mic_latency [--list] [--capture N] [--playback N] [--modes normal,low,exclusive]
 *                    [--gain G] [--out results.json] [--selftest]
 * Put the microphone close to a speaker (or a headphone against it), or use a loopback cable from an
 * output to an input. Medium volume is enough. */
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DECODING
#define MA_NO_ENCODING
#include "miniaudio.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double g_fake_time = -1;  /* --selftest: simulated clock */

#ifdef _WIN32
#include <windows.h>
static double real_now_s(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
#else
#include <time.h>
#include <unistd.h>
static double real_now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}
static void sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }
#endif
static double now_s(void) { return g_fake_time >= 0 ? g_fake_time : real_now_s(); }

#define CHIRP_MS 5.0
#define MAX_REC_S 12
#define MAX_BLOCKS 20000
#define MAX_BURSTS 64

typedef struct {
    ma_uint64 start;    /* index of the block's first frame in the recording */
    ma_uint32 frames;
    double    time;     /* callback entry */
} block_rec;

typedef struct {
    /* shared setup */
    ma_uint32 rate;
    float*    chirp;
    ma_uint32 chirp_len;
    int       monitor;          /* 0 = round trip test, 1 = monitor loop test */
    float     monitor_gain;
    ma_uint32 channel;          /* capture channel used */
    ma_uint32 cap_channels;
    /* capture side (capture thread only, read after stop) */
    float*    rec;
    ma_uint64 rec_cap, rec_len;
    block_rec blocks[MAX_BLOCKS];
    ma_uint32 block_count;
    ma_pcm_rb ring;
    /* playback side (playback thread only, read after stop) */
    ma_uint64 played;
    double    burst_time[MAX_BURSTS];   /* when each chirp was written, as if played at once */
    ma_uint32 burst_count;
    ma_uint64 inject_at;                /* monitor test: frame of the single chirp */
    int       started;                  /* monitor test: ring prefilled */
    ma_uint32 prefill;
    ma_uint32 underruns;
    double    fill_sum;
    ma_uint32 fill_n;
} spike;

static void on_capture(ma_device* d, void* out, const void* in, ma_uint32 n)
{
    spike* s = (spike*)d->pUserData;
    const float* x = (const float*)in;
    double t = now_s();
    ma_uint32 i;
    (void)out;
    if (s->block_count < MAX_BLOCKS) {
        block_rec* b = &s->blocks[s->block_count++];
        b->start = s->rec_len;
        b->frames = n;
        b->time = t;
    }
    for (i = 0; i < n; i++) {
        float v = x[(size_t)i * s->cap_channels + s->channel];
        if (s->rec_len < s->rec_cap) s->rec[s->rec_len++] = v;
    }
    if (s->monitor) {
        ma_uint32 left = n, done = 0;
        while (left) {
            ma_uint32 k = left;
            void* p;
            if (ma_pcm_rb_acquire_write(&s->ring, &k, &p) != MA_SUCCESS || k == 0) break;
            for (i = 0; i < k; i++) ((float*)p)[i] = x[(size_t)(done + i) * s->cap_channels + s->channel];
            ma_pcm_rb_commit_write(&s->ring, k);
            left -= k;
            done += k;
        }
    }
}

static void on_playback(ma_device* d, void* out, const void* in, ma_uint32 n)
{
    spike* s = (spike*)d->pUserData;
    float* y = (float*)out;
    ma_uint32 ch = d->playback.channels, i, c;
    double t = now_s();
    (void)in;
    for (i = 0; i < n * ch; i++) y[i] = 0.0f;

    if (s->monitor) {
        ma_uint32 avail = ma_pcm_rb_available_read(&s->ring);
        if (!s->started && avail >= s->prefill) s->started = 1;
        if (s->started) {
            ma_uint32 got = 0;
            s->fill_sum += avail;
            s->fill_n++;
            while (got < n) {
                ma_uint32 k = n - got;
                void* p;
                if (ma_pcm_rb_acquire_read(&s->ring, &k, &p) != MA_SUCCESS || k == 0) break;
                for (i = 0; i < k; i++) {
                    float v = ((const float*)p)[i] * s->monitor_gain;
                    if (v > 0.9f) v = 0.9f;
                    if (v < -0.9f) v = -0.9f;
                    for (c = 0; c < ch; c++) y[(size_t)(got + i) * ch + c] = v;
                }
                ma_pcm_rb_commit_read(&s->ring, k);
                got += k;
            }
            if (got < n) s->underruns++;
        }
        /* the single chirp */
        for (i = 0; i < n; i++) {
            ma_uint64 f = s->played + i;
            if (f >= s->inject_at && f < s->inject_at + s->chirp_len) {
                for (c = 0; c < ch; c++) y[(size_t)i * ch + c] += s->chirp[f - s->inject_at];
                if (f == s->inject_at && s->burst_count < MAX_BURSTS) s->burst_time[s->burst_count++] = t + (double)i / s->rate;
            }
        }
    } else {
        ma_uint64 every = s->rate / 2;
        for (i = 0; i < n; i++) {
            ma_uint64 f = s->played + i;
            ma_uint64 ph;
            if (f < s->rate / 4) continue;          /* let both devices settle first */
            ph = (f - s->rate / 4) % every;
            if (ph < s->chirp_len) {
                for (c = 0; c < ch; c++) y[(size_t)i * ch + c] = s->chirp[ph];
                if (ph == 0 && s->burst_count < MAX_BURSTS) s->burst_time[s->burst_count++] = t + (double)i / s->rate;
            }
        }
    }
    s->played += n;
}

/* Normalized cross-correlation of the recording with the chirp. */
static float* correlate(const spike* s, ma_uint64* out_n)
{
    ma_uint64 n = s->rec_len > s->chirp_len ? s->rec_len - s->chirp_len : 0, i;
    float* r = (float*)calloc((size_t)(n ? n : 1), sizeof(float));
    double ce = 0, we = 0;
    ma_uint32 k;
    for (k = 0; k < s->chirp_len; k++) ce += (double)s->chirp[k] * s->chirp[k];
    for (k = 0; k < s->chirp_len && k < s->rec_len; k++) we += (double)s->rec[k] * s->rec[k];
    for (i = 0; i < n; i++) {
        double acc = 0;
        for (k = 0; k < s->chirp_len; k++) acc += (double)s->rec[i + k] * s->chirp[k];
        r[i] = (float)(we > 1e-12 ? acc / sqrt(ce * we) : 0.0);
        we += (double)s->rec[i + s->chirp_len] * s->rec[i + s->chirp_len] - (double)s->rec[i] * s->rec[i];
        if (we < 0) we = 0;
    }
    *out_n = n;
    return r;
}

/* Peaks of the correlation above `thr`, at least `gap` frames apart, with their amplitude ratio
 * (recorded chirp / played chirp). */
static ma_uint32 find_peaks(const spike* s, const float* r, ma_uint64 n, float thr, ma_uint64 gap, ma_uint64 from,
                            ma_uint64* pos, float* amp, ma_uint32 max)
{
    ma_uint32 count = 0;
    ma_uint64 i = from;
    double ce = 0;
    ma_uint32 k;
    for (k = 0; k < s->chirp_len; k++) ce += (double)s->chirp[k] * s->chirp[k];
    while (i < n && count < max) {
        if (r[i] >= thr) {
            ma_uint64 best = i, j, end = i + s->chirp_len * 2 < n ? i + s->chirp_len * 2 : n;
            double acc = 0;
            for (j = i; j < end; j++)
                if (r[j] > r[best]) best = j;
            for (k = 0; k < s->chirp_len; k++) acc += (double)s->rec[best + k] * s->chirp[k];
            pos[count] = best;
            amp[count] = (float)fabs(acc / ce);
            count++;
            i = best + gap;
        } else {
            i++;
        }
    }
    return count;
}

/* When the recording's frame `f` reached the app's capture callback minus its place in the block:
 * the time it was captured. */
static double capture_time(const spike* s, ma_uint64 f)
{
    ma_uint32 i;
    for (i = 0; i < s->block_count; i++) {
        const block_rec* b = &s->blocks[i];
        if (f >= b->start && f < b->start + b->frames) return b->time - (double)(b->start + b->frames - f) / s->rate;
    }
    return -1;
}

static int cmp_d(const void* a, const void* b)
{
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

typedef struct {
    const char* name;
    int ok;
    char error[160];
    ma_uint32 rate, cap_period, cap_periods, play_period, play_periods;
    int low_latency_shared;     /* IAudioClient3 used (from the log) */
    int rt_n;
    double rt_median, rt_min, rt_max;
    float amp;
    int loop_n;
    double loop_median, loop_min, loop_max;
    double ring_fill_ms;
    float loop_match;           /* correlation of the first repeat with the first arrival */
    ma_uint32 underruns;
    float gain;
} result;

static int g_iac3_count;
static void on_log(void* user, ma_uint32 level, const char* msg)
{
    (void)user;
    (void)level;
    if (strstr(msg, "Using IAudioClient3")) g_iac3_count++;
    if (getenv("MIC_LATENCY_VERBOSE")) fprintf(stderr, "%s", msg);
}

static void make_chirp(spike* s)
{
    ma_uint32 k, rate = s->rate;
    s->chirp_len = (ma_uint32)(rate * CHIRP_MS / 1000.0);
    s->chirp = (float*)malloc(sizeof(float) * s->chirp_len);
    for (k = 0; k < s->chirp_len; k++) {
        double t = (double)k / rate, T = CHIRP_MS / 1000.0;
        double f0 = 1000, f1 = 6000;
        double ph = 2 * 3.14159265358979 * (f0 * t + (f1 - f0) * t * t / (2 * T));
        double w = 0.5 - 0.5 * cos(2 * 3.14159265358979 * k / (s->chirp_len - 1));
        s->chirp[k] = (float)(0.5 * w * sin(ph));
    }
}

static void analyze(spike* s, int pass, result* res)
{
    ma_uint32 rate = s->rate;
    ma_uint64 n = 0;
    float* cor = correlate(s, &n);
    if (pass == 0) {
        ma_uint64 pos[MAX_BURSTS];
        float amp[MAX_BURSTS];
        double rts[MAX_BURSTS];
        ma_uint32 np = find_peaks(s, cor, n, 0.5f, rate / 5, 0, pos, amp, MAX_BURSTS), i;
        float amps = 0;
        for (i = 0; i < np; i++) {
            double tc = capture_time(s, pos[i]), best = -1;
            ma_uint32 b;
            for (b = 0; b < s->burst_count; b++) {
                double d = tc - s->burst_time[b];
                if (d > 0 && d < 0.4 && (best < 0 || d < best)) best = d;
            }
            if (best > 0) {
                rts[res->rt_n++] = best * 1000.0;
                amps += amp[i];
            }
        }
        if (res->rt_n) {
            qsort(rts, (size_t)res->rt_n, sizeof(double), cmp_d);
            res->rt_median = rts[res->rt_n / 2];
            res->rt_min = rts[0];
            res->rt_max = rts[res->rt_n - 1];
            res->amp = amps / res->rt_n;
        }
    } else {
        /* The chirp's first arrival, then its repeats through the monitor. Each pass through the
         * speaker, the room and the mic colours it again, so the repeats are found by correlating
         * with what was recorded the first time (40 ms from the arrival, room echoes included)
         * rather than with the clean chirp. A repeat cannot come sooner than one capture plus one
         * playback period. */
        ma_uint64 p0;
        float a0;
        ma_uint32 np = find_peaks(s, cor, n, 0.5f, s->chirp_len * 2, rate / 2, &p0, &a0, 1), i;
        if (np) {
            ma_uint32 w = rate / 25;
            ma_uint64 min_lag = (ma_uint64)res->cap_period + res->play_period, max_lag = rate * 2 / 5, at = p0;
            double d[8];
            if (min_lag < rate / 200) min_lag = rate / 200;
            for (i = 0; i < 4 && at + max_lag + w < s->rec_len; i++) {
                const float* t = s->rec + at;
                double et = 0, best = 0;
                ma_uint64 lag, best_lag = 0;
                ma_uint32 k;
                for (k = 0; k < w; k++) et += (double)t[k] * t[k];
                for (lag = min_lag; lag <= max_lag; lag++) {
                    const float* x = t + lag;
                    double acc = 0, ex = 0, c;
                    for (k = 0; k < w; k++) {
                        acc += (double)t[k] * x[k];
                        ex += (double)x[k] * x[k];
                    }
                    c = ex > 1e-12 && et > 1e-12 ? acc / sqrt(et * ex) : 0;
                    if (c > best) {
                        best = c;
                        best_lag = lag;
                    }
                }
                if (best < 0.3) break;
                d[res->loop_n++] = (double)best_lag * 1000.0 / rate;
                if (i == 0) res->loop_match = (float)best;
                at += best_lag;
            }
            if (res->loop_n) {
                qsort(d, (size_t)res->loop_n, sizeof(double), cmp_d);
                res->loop_median = d[res->loop_n / 2];
                res->loop_min = d[0];
                res->loop_max = d[res->loop_n - 1];
            }
        }
        res->ring_fill_ms = s->fill_n ? s->fill_sum / s->fill_n * 1000.0 / rate : 0;
        res->underruns = s->underruns;
    }
    free(cor);
}

static ma_result open_pair(ma_context* ctx, const char* mode, ma_device_id* cap_id, ma_device_id* play_id,
                           spike* s, ma_device* cap, ma_device* play, ma_uint32 rate)
{
    ma_device_config pc = ma_device_config_init(ma_device_type_playback);
    ma_device_config cc = ma_device_config_init(ma_device_type_capture);
    ma_result r;
    pc.playback.pDeviceID = play_id;
    pc.playback.format = ma_format_f32;
    pc.playback.channels = 2;
    pc.sampleRate = rate;
    pc.dataCallback = on_playback;
    pc.pUserData = s;
    pc.performanceProfile = ma_performance_profile_low_latency;
    cc.capture.pDeviceID = cap_id;
    cc.capture.format = ma_format_f32;
    cc.capture.channels = 0;                 /* the device's own channel count */
    cc.sampleRate = rate;
    cc.dataCallback = on_capture;
    cc.pUserData = s;
    cc.performanceProfile = ma_performance_profile_low_latency;
    if (strcmp(mode, "low") == 0) {
        pc.periodSizeInFrames = cc.periodSizeInFrames = 16;  /* clamped up to the driver's minimum */
        pc.wasapi.noAutoConvertSRC = cc.wasapi.noAutoConvertSRC = MA_TRUE;
    } else if (strcmp(mode, "exclusive") == 0) {
        pc.periodSizeInMilliseconds = cc.periodSizeInMilliseconds = 3;
        pc.playback.shareMode = ma_share_mode_exclusive;
        cc.capture.shareMode = ma_share_mode_exclusive;
    }
    pc.wasapi.usage = cc.wasapi.usage = ma_wasapi_usage_pro_audio;
    r = ma_device_init(ctx, &cc, cap);
    if (r != MA_SUCCESS) return r;
    r = ma_device_init(ctx, &pc, play);
    if (r != MA_SUCCESS) {
        ma_device_uninit(cap);
        return r;
    }
    return MA_SUCCESS;
}

static void run_mode(ma_context* ctx, const char* mode, ma_device_id* cap_id, ma_device_id* play_id, result* res,
                     float gain_override)
{
    spike* s = (spike*)calloc(1, sizeof(spike));
    ma_device cap, play;
    ma_result r;
    int pass;
    memset(res, 0, sizeof(*res));
    res->name = mode;

    for (pass = 0; pass < 2; pass++) {
        ma_uint32 rate;
        memset(s, 0, sizeof(*s));
        s->monitor = pass;
        g_iac3_count = 0;
        /* rate 0 = native; the playback device's rate is then used for both */
        r = open_pair(ctx, mode, cap_id, play_id, s, &cap, &play, res->rate);
        if (r == MA_SUCCESS && play.sampleRate != cap.sampleRate) {
            ma_uint32 pr = play.sampleRate;
            ma_device_uninit(&play);
            ma_device_uninit(&cap);
            r = open_pair(ctx, mode, cap_id, play_id, s, &cap, &play, pr);
        }
        if (r != MA_SUCCESS) {
            snprintf(res->error, sizeof(res->error), "device open failed: %s", ma_result_description(r));
            free(s);
            return;
        }
        rate = play.sampleRate;
        res->rate = rate;
        res->cap_period = cap.capture.internalPeriodSizeInFrames;
        res->cap_periods = cap.capture.internalPeriods;
        res->play_period = play.playback.internalPeriodSizeInFrames;
        res->play_periods = play.playback.internalPeriods;
        if (pass == 0) res->low_latency_shared = g_iac3_count >= 2;
        s->rate = rate;
        s->cap_channels = cap.capture.channels;
        s->channel = 0;
        make_chirp(s);
        s->rec_cap = (ma_uint64)rate * MAX_REC_S;
        s->rec = (float*)malloc(sizeof(float) * (size_t)s->rec_cap);
        if (pass == 1) {
            s->monitor_gain = gain_override > 0 ? gain_override
                              : res->amp > 1e-4f ? (0.5f / res->amp > 30.0f ? 30.0f : 0.5f / res->amp) : 0.0f;
            res->gain = s->monitor_gain;
            if (s->monitor_gain <= 0) {
                ma_device_uninit(&play);
                ma_device_uninit(&cap);
                free(s->chirp);
                free(s->rec);
                break;
            }
            ma_pcm_rb_init(ma_format_f32, 1, rate, NULL, NULL, &s->ring);
            s->prefill = res->cap_period;
            s->inject_at = rate;  /* 1 s in */
        }
        ma_device_start(&cap);
        ma_device_start(&play);
        sleep_ms(pass == 0 ? 5500 : 3500);
        ma_device_stop(&play);
        ma_device_stop(&cap);
        ma_device_uninit(&play);
        ma_device_uninit(&cap);

        analyze(s, pass, res);
        if (pass == 1) ma_pcm_rb_uninit(&s->ring);
        free(s->chirp);
        free(s->rec);
        sleep_ms(300);
    }
    res->ok = res->rt_n > 0;
    if (!res->ok && !res->error[0]) snprintf(res->error, sizeof(res->error), "chirp not found in the recording (mic too far or muted?)");
    free(s);
}

/* Runs both tests on simulated devices: 480-frame periods at 48 kHz, the speaker heard by the mic
 * `delay` frames later at 0.3x with two room echoes and a little noise, no device latency. Expected: round trip =
 * delay, monitor loop = delay + the time audio waits in the ring (here one period: a capture block
 * only arrives at its end). */
static int selftest(void)
{
    const ma_uint32 rate = 48000, period = 480, delay = 1000;
    int pass, fails = 0;
    result res;
    memset(&res, 0, sizeof(res));
    res.name = "selftest";
    res.cap_period = res.play_period = period;
    for (pass = 0; pass < 2; pass++) {
        spike* s = (spike*)calloc(1, sizeof(spike));
        ma_device play, cap;
        float *line, out[480 * 2], in[480];
        ma_uint64 total = (ma_uint64)rate * (pass ? 3 : 5), f, line_len;
        ma_uint32 j, i;
        unsigned seed = 1;
        memset(&play, 0, sizeof(play));
        memset(&cap, 0, sizeof(cap));
        play.pUserData = cap.pUserData = s;
        play.playback.channels = 2;
        s->rate = rate;
        s->monitor = pass;
        s->cap_channels = 1;
        make_chirp(s);
        s->rec_cap = (ma_uint64)rate * MAX_REC_S;
        s->rec = (float*)malloc(sizeof(float) * (size_t)s->rec_cap);
        if (pass) {
            s->monitor_gain = 0.5f / res.amp;
            ma_pcm_rb_init(ma_format_f32, 1, rate, NULL, NULL, &s->ring);
            s->prefill = period;
            s->inject_at = rate;
        }
        line_len = total + delay + period + 1000;
        line = (float*)calloc((size_t)line_len, sizeof(float));
        for (j = 0; (ma_uint64)(j + 1) * period <= total; j++) {
            g_fake_time = (double)j * period / rate;
            on_playback(&play, out, NULL, period);
            for (i = 0; i < period; i++) {
                size_t at = (size_t)j * period + i + delay;
                line[at] += 0.3f * out[i * 2];
                line[at + 150] -= 0.12f * out[i * 2];   /* room echoes: 3 ms and 15 ms */
                line[at + 720] += 0.06f * out[i * 2];
            }
            for (i = 0; i < period; i++) {
                seed = seed * 1103515245u + 12345u;
                f = (ma_uint64)j * period + i;
                in[i] = line[f] + ((float)((seed >> 16) & 0x7fff) / 32768.0f - 0.5f) * 0.002f;
            }
            g_fake_time = (double)(j + 1) * period / rate;
            on_capture(&cap, NULL, in, period);
        }
        analyze(s, pass, &res);
        if (pass) ma_pcm_rb_uninit(&s->ring);
        free(line);
        free(s->chirp);
        free(s->rec);
        free(s);
    }
    g_fake_time = -1;
    printf("selftest: round trip %.2f ms (expected %.2f, %d chirps), monitor loop %.2f ms (expected about %.2f, %d repeats), ring %.2f ms, underruns %u\n",
           res.rt_median, delay * 1000.0 / rate, res.rt_n, res.loop_median, delay * 1000.0 / rate + res.ring_fill_ms,
           res.loop_n, res.ring_fill_ms, res.underruns);
    if (res.rt_n < 8 || fabs(res.rt_median - delay * 1000.0 / rate) > 0.2) fails++;
    if (res.loop_n < 2 || fabs(res.loop_median - (delay * 1000.0 / rate + res.ring_fill_ms)) > 0.5) fails++;
    printf("selftest %s\n", fails ? "FAILED" : "passed");
    return fails ? 1 : 0;
}

int main(int argc, char** argv)
{
    ma_context ctx;
    ma_context_config cfg = ma_context_config_init();
    ma_log log;
    ma_device_info *pinfo, *cinfo;
    ma_uint32 pcount, ccount, i;
    int cap_index = -1, play_index = -1, list = 0;
    const char* modes = "normal,low,exclusive";
    const char* out_path = "mic-latency-results.json";
    float gain = 0;
    char buf[128], *tok;
    result res[3];
    int nres = 0;
    FILE* f;

    for (i = 1; i < (ma_uint32)argc; i++) {
        if (!strcmp(argv[i], "--selftest")) return selftest();
        else if (!strcmp(argv[i], "--list")) list = 1;
        else if (!strcmp(argv[i], "--capture") && i + 1 < (ma_uint32)argc) cap_index = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--playback") && i + 1 < (ma_uint32)argc) play_index = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--modes") && i + 1 < (ma_uint32)argc) modes = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < (ma_uint32)argc) out_path = argv[++i];
        else if (!strcmp(argv[i], "--gain") && i + 1 < (ma_uint32)argc) gain = (float)atof(argv[++i]);
        else {
            fprintf(stderr, "usage: %s [--list] [--capture N] [--playback N] [--modes normal,low,exclusive] [--gain G] [--out file]\n", argv[0]);
            return 2;
        }
    }

    ma_log_init(NULL, &log);
    ma_log_register_callback(&log, ma_log_callback_init(on_log, NULL));
    cfg.pLog = &log;
    if (ma_context_init(NULL, 0, &cfg, &ctx) != MA_SUCCESS) {
        fprintf(stderr, "no audio backend\n");
        return 1;
    }
    ma_context_get_devices(&ctx, &pinfo, &pcount, &cinfo, &ccount);
    printf("backend: %s\n", ma_get_backend_name(ctx.backend));
    printf("playback devices:\n");
    for (i = 0; i < pcount; i++) printf("  [%u]%s %s\n", i, pinfo[i].isDefault ? " (default)" : "", pinfo[i].name);
    printf("capture devices:\n");
    for (i = 0; i < ccount; i++) printf("  [%u]%s %s\n", i, cinfo[i].isDefault ? " (default)" : "", cinfo[i].name);
    if (list) {
        ma_context_uninit(&ctx);
        return 0;
    }
    if (cap_index >= (int)ccount || play_index >= (int)pcount) {
        fprintf(stderr, "no such device\n");
        return 2;
    }

    snprintf(buf, sizeof(buf), "%s", modes);
    for (tok = strtok(buf, ","); tok && nres < 3; tok = strtok(NULL, ",")) {
        const char* m = !strcmp(tok, "normal") ? "normal" : !strcmp(tok, "low") ? "low" : !strcmp(tok, "exclusive") ? "exclusive" : NULL;
        if (!m) continue;
        printf("\n== %s ==\n", m);
        fflush(stdout);
        run_mode(&ctx, m, cap_index >= 0 ? &cinfo[cap_index].id : NULL, play_index >= 0 ? &pinfo[play_index].id : NULL,
                 &res[nres], gain);
        {
            result* r = &res[nres];
            if (r->error[0] && !r->rate) {
                printf("  %s\n", r->error);
            } else {
                printf("  rate %u Hz, capture period %u x %u (%.2f ms), playback period %u x %u (%.2f ms), IAudioClient3: %s\n",
                       r->rate, r->cap_period, r->cap_periods, r->cap_period * 1000.0 / r->rate, r->play_period,
                       r->play_periods, r->play_period * 1000.0 / r->rate, r->low_latency_shared ? "yes" : "no");
                if (r->rt_n)
                    printf("  round trip (out + in + air): median %.1f ms, min %.1f, max %.1f (%d chirps), level %.3f\n",
                           r->rt_median, r->rt_min, r->rt_max, r->rt_n, r->amp);
                if (r->loop_n)
                    printf("  monitor loop (mic -> speaker + air): median %.1f ms, min %.1f, max %.1f (%d repeats, match %.2f), gain %.1f, ring %.1f ms, underruns %u\n",
                           r->loop_median, r->loop_min, r->loop_max, r->loop_n, r->loop_match, r->gain, r->ring_fill_ms, r->underruns);
                else if (r->rt_n)
                    printf("  monitor loop: no repeats found (gain %.1f); try --gain with a higher value\n", r->gain);
                if (r->error[0]) printf("  %s\n", r->error);
            }
        }
        nres++;
    }

    f = fopen(out_path, "w");
    if (f) {
        int j;
        fprintf(f, "{\n  \"backend\": \"%s\",\n  \"capture\": \"%s\",\n  \"playback\": \"%s\",\n  \"modes\": [\n",
                ma_get_backend_name(ctx.backend), cap_index >= 0 ? cinfo[cap_index].name : "default",
                play_index >= 0 ? pinfo[play_index].name : "default");
        for (j = 0; j < nres; j++) {
            result* r = &res[j];
            fprintf(f,
                    "    {\"mode\": \"%s\", \"error\": \"%s\", \"rate\": %u, \"capture_period\": %u, \"capture_periods\": %u, "
                    "\"playback_period\": %u, \"playback_periods\": %u, \"iaudioclient3\": %d, "
                    "\"round_trip_ms\": {\"n\": %d, \"median\": %.2f, \"min\": %.2f, \"max\": %.2f}, \"level\": %.4f, "
                    "\"monitor_loop_ms\": {\"n\": %d, \"median\": %.2f, \"min\": %.2f, \"max\": %.2f}, "
                    "\"loop_match\": %.2f, \"gain\": %.2f, \"ring_fill_ms\": %.2f, \"underruns\": %u}%s\n",
                    r->name, r->error, r->rate, r->cap_period, r->cap_periods, r->play_period, r->play_periods,
                    r->low_latency_shared, r->rt_n, r->rt_median, r->rt_min, r->rt_max, r->amp, r->loop_n,
                    r->loop_median, r->loop_min, r->loop_max, r->loop_match, r->gain, r->ring_fill_ms, r->underruns,
                    j + 1 < nres ? "," : "");
        }
        fprintf(f, "  ]\n}\n");
        fclose(f);
        printf("\nwrote %s\n", out_path);
    }
    ma_context_uninit(&ctx);
    return 0;
}
