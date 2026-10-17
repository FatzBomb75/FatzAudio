/* fatzaudio_ffmpeg.h against a real FFmpeg, the way an app uses it: libavformat
 * demuxes a video file (MPEG-4 video plus AC-3 audio), the audio packets go to a
 * packet stream untouched, and in the second test the app decodes them itself and
 * pushes the AVFrames. Only built when FFmpeg's development files are found. */
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <fatzaudio/fatzaudio.h>
#include <fatzaudio/fatzaudio_ffmpeg.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef FA_TEST_DATA_DIR
#  define FA_TEST_DATA_DIR "data"
#endif

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; goto done; } } while (0)
#define RUN(t) do { int before_ = g_failures; printf("%s\n", #t); t(); if (g_failures == before_) printf("  ok\n"); } while (0)

static fa_engine* g_engine;

typedef struct demux {
    AVFormatContext* fmt;
    int audio;
    AVStream* st;
} demux;

static int demux_open(demux* d)
{
    memset(d, 0, sizeof(*d));
    if (avformat_open_input(&d->fmt, FA_TEST_DATA_DIR "/ffmpeg_av.mkv", NULL, NULL) < 0) return 0;
    if (avformat_find_stream_info(d->fmt, NULL) < 0) return 0;
    d->audio = av_find_best_stream(d->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (d->audio < 0) return 0;
    d->st = d->fmt->streams[d->audio];
    return 1;
}

/* Reads a decode-only stream to the end; returns frames, fills the RMS of the middle half. */
static long long drain(fa_stream* s, uint32_t channels, double* out_rms)
{
    long long cap = 1 << 16, n = 0, i;
    float* all = (float*)malloc((size_t)cap * channels * sizeof(float));
    double acc = 0.0;
    for (;;) {
        fa_frames got = 0;
        fa_result r;
        if (n + 1024 > cap) { cap *= 2; all = (float*)realloc(all, (size_t)cap * channels * sizeof(float)); }
        r = fa_stream_read(s, all + n * channels, 1024, &got);
        n += (long long)got;
        if (r == FA_ERROR_END_OF_STREAM) break;
        if (r != FA_OK) { n = -1; break; }
    }
    if (n > 4) {
        for (i = n / 4 * channels; i < n * 3 / 4 * channels; i++) acc += (double)all[i] * all[i];
        *out_rms = sqrt(acc / (double)((n * 3 / 4 - n / 4) * channels));
    }
    free(all);
    return n;
}

static void test_packets_from_av_read_frame(void)
{
    demux d;
    fa_codec_params params;
    fa_open_options o;
    fa_stream* s = NULL;
    AVPacket* pkt = av_packet_alloc();
    long long n;
    double level = 0.0;
    fa_time clock = 0;
    CHECK(demux_open(&d));
    fa_codec_params_from_av(&params, d.st->codecpar, d.st->time_base);
    CHECK(strcmp(params.codec, "ac3") == 0);
    CHECK(params.sample_rate == 48000 && params.channels == 2);
    CHECK(params.time_base.num == 1 && params.time_base.den == 1000);
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    CHECK(fa_stream_open_packets(g_engine, &params, &o, &s) == FA_OK);
    CHECK(fa_stream_get_clock(s, &clock) == FA_OK);
    while (av_read_frame(d.fmt, pkt) >= 0) {
        if (pkt->stream_index == d.audio) {
            fa_packet p;
            fa_packet_from_av(&p, pkt);
            if (fa_stream_push_packet(s, &p) != FA_OK) { av_packet_unref(pkt); CHECK(0); }
        }
        av_packet_unref(pkt);
    }
    CHECK(fa_stream_push_end(s) == FA_OK);
    n = drain(s, 2, &level);
    /* 0.5 s of AC-3: 16 frames of 1536 samples, the last one padded. */
    CHECK(n >= 24000 && n <= 24576 + 1536);
    CHECK(fabs(level - 0.3536) < 0.05);
done:
    if (s) fa_stream_close(s);
    av_packet_free(&pkt);
    if (d.fmt) avformat_close_input(&d.fmt);
}

static void test_frames_from_the_apps_decoder(void)
{
    demux d;
    fa_audio_format want;
    fa_open_options o;
    fa_stream* s = NULL;
    const AVCodec* codec;
    AVCodecContext* dec = NULL;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    long long n;
    double level = 0.0;
    int pushed = 0;
    CHECK(demux_open(&d));
    codec = avcodec_find_decoder(d.st->codecpar->codec_id);
    CHECK(codec != NULL);
    dec = avcodec_alloc_context3(codec);
    CHECK(dec != NULL);
    CHECK(avcodec_parameters_to_context(dec, d.st->codecpar) >= 0);
    dec->pkt_timebase = d.st->time_base;
    CHECK(avcodec_open2(dec, codec, NULL) >= 0);
    /* The stream plays at 44.1 kHz mono: the planar float frames are converted on push. */
    memset(&want, 0, sizeof(want));
    want.sample_format = FA_FORMAT_F32;
    want.sample_rate = 44100;
    want.channels = 1;
    fa_open_options_init(&o);
    o.flags = FA_OPEN_DECODE_ONLY;
    CHECK(fa_stream_open_push(g_engine, &want, &o, &s) == FA_OK);
    while (av_read_frame(d.fmt, pkt) >= 0) {
        if (pkt->stream_index == d.audio && avcodec_send_packet(dec, pkt) >= 0) {
            while (avcodec_receive_frame(dec, frame) >= 0) {
                fa_audio_frame f;
                if (fa_audio_frame_from_av(&f, frame, d.st->time_base) != FA_OK ||
                    fa_stream_push_frame(s, &f) != FA_OK) { av_frame_unref(frame); av_packet_unref(pkt); CHECK(0); }
                pushed++;
                av_frame_unref(frame);
            }
        }
        av_packet_unref(pkt);
    }
    CHECK(pushed >= 15);  /* 1536-sample AC-3 frames */
    CHECK(fa_stream_push_end(s) == FA_OK);
    n = drain(s, 1, &level);
    CHECK(n >= 22050 - 1500 && n <= 22050 + 1500);
    CHECK(fabs(level - 0.3536) < 0.05);
done:
    if (s) fa_stream_close(s);
    if (dec) avcodec_free_context(&dec);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    if (d.fmt) avformat_close_input(&d.fmt);
}

static void test_sample_format_map(void)
{
    CHECK(fa_sample_format_from_av(AV_SAMPLE_FMT_FLTP) == FA_FORMAT_F32_PLANAR);
    CHECK(fa_sample_format_from_av(AV_SAMPLE_FMT_S16) == FA_FORMAT_S16);
    CHECK(fa_sample_format_from_av(AV_SAMPLE_FMT_DBLP) == FA_FORMAT_F64_PLANAR);
    CHECK(fa_sample_format_from_av(AV_SAMPLE_FMT_S64) == -1);
done:;
}

int main(void)
{
    fa_engine_config cfg;
    fa_engine_config_init(&cfg);
    cfg.flags = FA_ENGINE_NULL_OUTPUT;
    if (fa_engine_create(&cfg, &g_engine) != FA_OK) { fprintf(stderr, "engine\n"); return 1; }
    RUN(test_sample_format_map);
    RUN(test_packets_from_av_read_frame);
    RUN(test_frames_from_the_apps_decoder);
    fa_engine_destroy(g_engine);
    if (g_failures) { fprintf(stderr, "%d failure(s)\n", g_failures); return 1; }
    printf("all passed\n");
    return 0;
}
