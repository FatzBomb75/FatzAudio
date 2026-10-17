/* miniaudio implementation lives in its own translation unit. miniaudio embeds
 * dr_wav / dr_mp3 / dr_flac, so WAV, MP3 and FLAC decoding come from it. */
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

/* ---- dr_wav glue for dec_miniaudio.c --------------------------------------- */
#include "fa_internal.h"
#include <stdlib.h>

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
        if (src->length < 0 || !src->seekable) return MA_FALSE;
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
