/* miniaudio implementation lives in its own translation unit. miniaudio embeds
 * dr_wav / dr_mp3 / dr_flac, so WAV, MP3 and FLAC decoding come from it. */
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

/* ---- dr_wav glue for dec_miniaudio.c --------------------------------------- */
#include "fa_internal.h"
#include <stdlib.h>
#include <string.h>

static size_t wav_on_read(void* user, void* buf, size_t bytes)
{
    int64_t r = fa_source_read((fa_source*)user, buf, bytes);
    return r > 0 ? (size_t)r : 0;
}

static ma_bool32 wav_on_seek(void* user, int offset, ma_dr_wav_seek_origin origin)
{
    fa_source* src = (fa_source*)user;
    int64_t target = offset;
    if (origin == MA_DR_WAV_SEEK_CUR) target = fa_source_tell(src) + offset;
    else if (origin == MA_DR_WAV_SEEK_END) {
        /* dr_wav only asks for the end to learn the file size. Over the network
         * that seek would restart the download twice; skip it. */
        if (src->length < 0 || !src->seekable || src->remote) return MA_FALSE;
        target = src->length + offset;
    }
    return fa_source_seek(src, target) == FA_OK;
}

static ma_bool32 wav_on_tell(void* user, ma_int64* cursor)
{
    *cursor = fa_source_tell((fa_source*)user);
    return MA_TRUE;
}

fa_result fa_drwav_open(fa_source* src, int sequential, void** out_impl,
                        uint32_t* out_rate, uint32_t* out_channels, uint64_t* out_frames)
{
    ma_dr_wav* w = (ma_dr_wav*)calloc(1, sizeof(ma_dr_wav));
    if (!w) return FA_ERROR_OUT_OF_MEMORY;
    if (!ma_dr_wav_init_ex(w, wav_on_read, wav_on_seek, wav_on_tell, NULL, src, NULL,
                           sequential ? MA_DR_WAV_SEQUENTIAL : 0, NULL)) {
        free(w);
        return FA_ERROR_UNSUPPORTED_FORMAT;
    }
    if (w->sampleRate == 0 || w->channels == 0) {
        ma_dr_wav_uninit(w);
        free(w);
        return FA_ERROR_UNSUPPORTED_FORMAT;
    }
    *out_impl = w;
    *out_rate = w->sampleRate;
    *out_channels = w->channels;
    *out_frames = w->totalPCMFrameCount;
    return FA_OK;
}

uint64_t fa_drwav_read(void* impl, float* out, uint64_t frames)
{
    return ma_dr_wav_read_pcm_frames_f32((ma_dr_wav*)impl, frames, out);
}

int fa_drwav_seek(void* impl, uint64_t frame)
{
    return ma_dr_wav_seek_to_pcm_frame((ma_dr_wav*)impl, frame) ? 1 : 0;
}

void fa_drwav_close(void* impl)
{
    if (!impl) return;
    ma_dr_wav_uninit((ma_dr_wav*)impl);
    free(impl);
}

/* ---- dr_mp3 glue for MP3 over the network (dec_miniaudio.c) -----------------
 * No tell callback: dr_mp3 then never seeks to the end of the stream looking for
 * ID3v1/APE tags, which over HTTP would cost two extra requests at open. Seeks
 * are relative to where the decoder was (re)started, so it can be restarted at
 * any byte of the file. */

typedef struct fa_drmp3 {
    ma_dr_mp3 dr;
    fa_source* src;
    int64_t base;
    int live;                    /* dr is initialised (a failed restart leaves it not) */
} fa_drmp3;

static size_t mp3_on_read(void* user, void* buf, size_t bytes)
{
    int64_t r = fa_source_read(((fa_drmp3*)user)->src, buf, bytes);
    return r > 0 ? (size_t)r : 0;
}

static ma_bool32 mp3_on_seek(void* user, int offset, ma_dr_mp3_seek_origin origin)
{
    fa_drmp3* m = (fa_drmp3*)user;
    int64_t target;
    if (origin == MA_DR_MP3_SEEK_SET) target = m->base + offset;
    else if (origin == MA_DR_MP3_SEEK_CUR) target = fa_source_tell(m->src) + offset;
    else return MA_FALSE;
    return fa_source_seek(m->src, target) == FA_OK;
}

static int drmp3_start(fa_drmp3* m)
{
    m->base = fa_source_tell(m->src);
    return ma_dr_mp3_init(&m->dr, mp3_on_read, mp3_on_seek, NULL, NULL, m, NULL) ? 1 : 0;
}

fa_result fa_drmp3_open(fa_source* src, void** out_impl, uint32_t* out_rate, uint32_t* out_channels,
                        uint64_t* out_frames, int64_t* out_data_start)
{
    fa_drmp3* m = (fa_drmp3*)calloc(1, sizeof(fa_drmp3));
    if (!m) return FA_ERROR_OUT_OF_MEMORY;
    m->src = src;
    if (!drmp3_start(m)) { free(m); return FA_ERROR_UNSUPPORTED_FORMAT; }
    if (m->dr.channels == 0 || m->dr.sampleRate == 0) { ma_dr_mp3_uninit(&m->dr); free(m); return FA_ERROR_UNSUPPORTED_FORMAT; }
    m->live = 1;
    *out_impl = m;
    *out_rate = m->dr.sampleRate;
    *out_channels = m->dr.channels;
    /* Only the Xing/Info frame count: counting frames would read the whole file. */
    *out_frames = m->dr.totalPCMFrameCount != MA_UINT64_MAX ? ma_dr_mp3_get_pcm_frame_count(&m->dr) : 0;
    *out_data_start = (int64_t)m->dr.streamStartOffset;
    return FA_OK;
}

uint64_t fa_drmp3_read(void* impl, float* out, uint64_t frames)
{
    fa_drmp3* m = (fa_drmp3*)impl;
    return m->live ? ma_dr_mp3_read_pcm_frames_f32(&m->dr, frames, out) : 0;
}

/* Starts decoding again at the source's current position (after a byte seek). */
fa_result fa_drmp3_restart(void* impl)
{
    fa_drmp3* m = (fa_drmp3*)impl;
    uint32_t rate = m->dr.sampleRate, channels = m->dr.channels;
    if (m->live) ma_dr_mp3_uninit(&m->dr);
    memset(&m->dr, 0, sizeof(m->dr));
    m->live = 0;
    if (!drmp3_start(m)) return FA_ERROR_IO;   /* reads now return the end */
    m->live = 1;
    if (m->dr.sampleRate != rate || m->dr.channels != channels) {
        ma_dr_mp3_uninit(&m->dr);
        m->live = 0;
        return FA_ERROR_UNSUPPORTED_FORMAT;
    }
    return FA_OK;
}

/* Bytes read from the source but not decoded yet, and frames decoded but not returned. */
void fa_drmp3_lag(void* impl, int64_t* out_bytes, int64_t* out_frames)
{
    fa_drmp3* m = (fa_drmp3*)impl;
    *out_bytes = m->live ? (int64_t)m->dr.dataSize : 0;
    *out_frames = m->live ? (int64_t)m->dr.pcmFramesRemainingInMP3Frame : 0;
}

void fa_drmp3_close(void* impl)
{
    if (!impl) return;
    if (((fa_drmp3*)impl)->live) ma_dr_mp3_uninit(&((fa_drmp3*)impl)->dr);
    free(impl);
}
