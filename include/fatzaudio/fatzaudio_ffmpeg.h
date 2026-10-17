/* fatzaudio_ffmpeg.h: one-line bridges from FFmpeg's structs to FatzAudio's.
 *
 * Header-only and optional. Include it after your own FFmpeg headers; it compiles
 * against whatever FFmpeg version your app uses (4.0 to 8.x), so FatzAudio itself
 * never depends on FFmpeg's headers or ABI.
 *
 *   #include <libavformat/avformat.h>
 *   #include <fatzaudio/fatzaudio.h>
 *   #include <fatzaudio/fatzaudio_ffmpeg.h>
 *
 *   AVStream* st = fmt->streams[audio_index];
 *   fa_codec_params params;
 *   fa_codec_params_from_av(&params, st->codecpar, st->time_base);
 *   fa_stream_open_packets(engine, &params, NULL, &audio);
 *   ...
 *   while (av_read_frame(fmt, pkt) >= 0) {
 *       if (pkt->stream_index == audio_index) {
 *           fa_packet p;
 *           fa_packet_from_av(&p, pkt);
 *           fa_stream_push_packet(audio, &p);
 *       }
 *       av_packet_unref(pkt);
 *   }
 *
 * Decoded frames instead (your own avcodec_receive_frame loop):
 *
 *   fa_audio_frame f;
 *   if (fa_audio_frame_from_av(&f, frame, st->time_base) == FA_OK)
 *       fa_stream_push_frame(audio, &f);
 *
 * The structs only point into FFmpeg's buffers; FatzAudio copies the data during
 * the push call, so the AVPacket or AVFrame can be unreferenced right after. */
#ifndef FATZAUDIO_FFMPEG_H
#define FATZAUDIO_FFMPEG_H

#include "fatzaudio.h"

#if !defined(AVCODEC_AVCODEC_H) && !defined(AVCODEC_CODEC_PAR_H)
#error "include <libavcodec/avcodec.h> (or <libavformat/avformat.h>) before fatzaudio_ffmpeg.h"
#endif

#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* AVCodecParameters->ch_layout replaced ->channels in libavutil 57.24 (FFmpeg 5.1). */
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 24, 100)
#define FA_AV_CHANNELS(x) ((x)->ch_layout.nb_channels)
#else
#define FA_AV_CHANNELS(x) ((x)->channels)
#endif

/* The fa_sample_format matching an AVSampleFormat (AVFrame->format), or -1 when
 * FatzAudio has none (AV_SAMPLE_FMT_S64 and S64P). */
static inline int fa_sample_format_from_av(int av_format)
{
    switch (av_format) {
    case AV_SAMPLE_FMT_U8:   return FA_FORMAT_U8;
    case AV_SAMPLE_FMT_S16:  return FA_FORMAT_S16;
    case AV_SAMPLE_FMT_S32:  return FA_FORMAT_S32;
    case AV_SAMPLE_FMT_FLT:  return FA_FORMAT_F32;
    case AV_SAMPLE_FMT_DBL:  return FA_FORMAT_F64;
    case AV_SAMPLE_FMT_S16P: return FA_FORMAT_S16_PLANAR;
    case AV_SAMPLE_FMT_S32P: return FA_FORMAT_S32_PLANAR;
    case AV_SAMPLE_FMT_FLTP: return FA_FORMAT_F32_PLANAR;
    case AV_SAMPLE_FMT_DBLP: return FA_FORMAT_F64_PLANAR;
    default:                 return -1; /* U8P is planar u8: not supported either */
    }
}

/* Describes an audio track for fa_stream_open_packets. time_base is the
 * AVStream's (st->time_base), the unit of the packets' pts. The codec name and
 * extradata point into FFmpeg's structs: open the stream before freeing them. */
static inline void fa_codec_params_from_av(fa_codec_params* out, const AVCodecParameters* par, AVRational time_base)
{
    fa_codec_params_init(out);
    out->codec = avcodec_get_name(par->codec_id);
    out->sample_rate = par->sample_rate > 0 ? (uint32_t)par->sample_rate : 0;
    out->channels = FA_AV_CHANNELS(par) > 0 ? (uint32_t)FA_AV_CHANNELS(par) : 0;
    out->extradata = par->extradata;
    out->extradata_size = par->extradata_size > 0 ? (size_t)par->extradata_size : 0;
    out->bit_rate = par->bit_rate;
    out->block_align = par->block_align > 0 ? (uint32_t)par->block_align : 0;
    out->bits_per_coded_sample = par->bits_per_coded_sample > 0 ? (uint32_t)par->bits_per_coded_sample : 0;
    out->time_base.num = time_base.num;
    out->time_base.den = time_base.den;
}

/* One packet from av_read_frame. AV_NOPTS_VALUE is FA_PTS_NONE, so it passes through. */
static inline void fa_packet_from_av(fa_packet* out, const AVPacket* pkt)
{
    out->data = pkt->data;
    out->size = pkt->size > 0 ? (size_t)pkt->size : 0;
    out->pts = pkt->pts;
    out->duration = pkt->duration > 0 ? pkt->duration : 0;
}

/* One decoded frame. pts is frame->pts, falling back to best_effort_timestamp,
 * in time_base (the AVStream's, or the decoder's pkt_timebase). Returns
 * FA_ERROR_UNSUPPORTED_FORMAT for sample formats FatzAudio has no layout for. */
static inline fa_result fa_audio_frame_from_av(fa_audio_frame* out, const AVFrame* frame, AVRational time_base)
{
    int fmt = fa_sample_format_from_av(frame->format);
    if (fmt < 0 || frame->sample_rate <= 0 || FA_AV_CHANNELS(frame) <= 0) return FA_ERROR_UNSUPPORTED_FORMAT;
    memset(out, 0, sizeof(*out));
    out->format.sample_format = (fa_sample_format)fmt;
    out->format.sample_rate = (uint32_t)frame->sample_rate;
    out->format.channels = (uint32_t)FA_AV_CHANNELS(frame);
    out->data = (const void* const*)frame->extended_data;
    out->frames = frame->nb_samples > 0 ? (fa_frames)frame->nb_samples : 0;
    out->pts = frame->pts != AV_NOPTS_VALUE ? frame->pts : frame->best_effort_timestamp;
    out->time_base.num = time_base.num;
    out->time_base.den = time_base.den;
    return FA_OK;
}

#ifdef __cplusplus
}
#endif

#endif /* FATZAUDIO_FFMPEG_H */
