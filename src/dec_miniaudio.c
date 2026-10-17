/* WAV/AIFF, FLAC and MP3 decoders. Each one drives miniaudio's embedded
 * dr_wav / dr_flac / dr_mp3 for exactly one format (no trial-and-error across
 * backends): the registry has already decided what the data is. */
#include "fa_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ma_result on_read(ma_decoder* d, void* buf, size_t bytes, size_t* out_read)
{
    fa_source* src = (fa_source*)d->pUserData;
    int64_t r = fa_source_read(src, buf, bytes);
    if (r < 0) { *out_read = 0; return MA_IO_ERROR; }
    *out_read = (size_t)r;
    return (r == 0 && bytes > 0) ? MA_AT_END : MA_SUCCESS;
}

static ma_result on_seek(ma_decoder* d, ma_int64 offset, ma_seek_origin origin)
{
    fa_source* src = (fa_source*)d->pUserData;
    int64_t target = offset;
    if (origin == ma_seek_origin_current) target = fa_source_tell(src) + offset;
    else if (origin == ma_seek_origin_end) {
        if (src->length < 0) return MA_NOT_IMPLEMENTED;
        target = src->length + offset;
    }
    return fa_source_seek(src, target) == FA_OK ? MA_SUCCESS : MA_BAD_SEEK;
}

static fa_result open_with(fa_decoder* dec, ma_encoding_format encoding)
{
    ma_decoder* md = (ma_decoder*)calloc(1, sizeof(ma_decoder));
    ma_decoder_config cfg;
    ma_uint64 len = 0;
    ma_result r;
    if (!md) return FA_ERROR_OUT_OF_MEMORY;
    cfg = ma_decoder_config_init(ma_format_f32, 0, 0); /* native channels and rate */
    cfg.encodingFormat = encoding;
    r = ma_decoder_init(on_read, on_seek, dec->src, &cfg, md);
    if (r != MA_SUCCESS) {
        free(md);
        if (r == MA_OUT_OF_MEMORY) return FA_ERROR_OUT_OF_MEMORY;
        return FA_ERROR_UNSUPPORTED_FORMAT;
    }
    dec->impl = md;
    dec->format.sample_rate = md->outputSampleRate;
    dec->format.channels = md->outputChannels;
    dec->seekable = dec->src->seekable;
    if (ma_decoder_get_length_in_pcm_frames(md, &len) == MA_SUCCESS && len > 0) dec->length = (fa_frames)len;
    if (dec->format.sample_rate == 0 || dec->format.channels == 0) {
        ma_decoder_uninit(md);
        free(md);
        dec->impl = NULL;
        return FA_ERROR_UNSUPPORTED_FORMAT;
    }
    return FA_OK;
}

static fa_result ma_read(fa_decoder* dec, float* out, fa_frames frames, fa_frames* got)
{
    ma_uint64 n = 0;
    ma_result r = ma_decoder_read_pcm_frames((ma_decoder*)dec->impl, out, (ma_uint64)frames, &n);
    *got = (fa_frames)n;
    if (n > 0) return FA_OK;
    return (r == MA_AT_END || r == MA_SUCCESS) ? FA_ERROR_END_OF_STREAM : FA_ERROR_IO;
}

static fa_result ma_seek(fa_decoder* dec, fa_frames frame)
{
    return ma_decoder_seek_to_pcm_frame((ma_decoder*)dec->impl, (ma_uint64)frame) == MA_SUCCESS
        ? FA_OK : FA_ERROR_NOT_SEEKABLE;
}

static void ma_close(fa_decoder* dec)
{
    if (dec->impl) {
        ma_decoder_uninit((ma_decoder*)dec->impl);
        free(dec->impl);
        dec->impl = NULL;
    }
}

static fa_result wav_open(fa_decoder* dec)
{
    const uint8_t* head;
    size_t n = fa_source_peek(dec->src, &head);
    fa_result r = open_with(dec, ma_encoding_format_wav);
    if (r == FA_OK) {
        int aiff = n >= 4 && memcmp(head, "FORM", 4) == 0;
        snprintf(dec->codec, sizeof(dec->codec), "pcm");
        snprintf(dec->container, sizeof(dec->container), "%s", aiff ? "aiff" : "wav");
    }
    return r;
}

static fa_result flac_open(fa_decoder* dec) { return open_with(dec, ma_encoding_format_flac); }
static fa_result mp3_open(fa_decoder* dec) { return open_with(dec, ma_encoding_format_mp3); }

const fa_decoder_vtable fa_decoder_wav = {
    "wav", "WAV, RF64, Wave64 and AIFF (PCM, float, ADPCM)", "wav;wave;rf64;w64;bwf;aif;aiff;aifc",
    "audio/wav;audio/x-wav;audio/wave;audio/vnd.wave;audio/aiff;audio/x-aiff",
    fa_probe_wav, wav_open, ma_read, ma_seek, ma_close
};

const fa_decoder_vtable fa_decoder_flac = {
    "flac", "Free Lossless Audio Codec", "flac;fla",
    "audio/flac;audio/x-flac",
    fa_probe_flac, flac_open, ma_read, ma_seek, ma_close
};

const fa_decoder_vtable fa_decoder_mp3 = {
    "mp3", "MPEG-1/2/2.5 Layer III", "mp3",
    "audio/mpeg;audio/mp3;audio/x-mp3",
    fa_probe_mp3, mp3_open, ma_read, ma_seek, ma_close
};
