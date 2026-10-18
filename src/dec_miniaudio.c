/* WAV/AIFF and FLAC decoders, and MP3 over the network or in free format (local
 * MP3 files are dec_mp3.c). Each one drives miniaudio's embedded
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
        /* Going to the end of a forward-only source would consume all of it. */
        if (src->length < 0 || !src->seekable) return MA_NOT_IMPLEMENTED;
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

/* WAV/AIFF use dr_wav directly (see miniaudio_impl.c) so a forward-only source
 * can be opened in sequential mode. Web streams open sequentially too: otherwise
 * dr_wav seeks past the audio for trailing chunks, a download restart each way. */
static fa_result wav_open(fa_decoder* dec)
{
    const uint8_t* head;
    size_t n = fa_source_peek(dec->src, &head);
    uint32_t rate = 0, channels = 0;
    uint64_t frames = 0;
    fa_result r = fa_drwav_open(dec->src, !dec->src->seekable || dec->src->remote, &dec->impl, &rate, &channels, &frames);
    if (r != FA_OK) return r;
    dec->format.sample_rate = rate;
    dec->format.channels = channels;
    dec->seekable = dec->src->seekable;
    if (frames > 0) dec->length = (fa_frames)frames;
    fa_drwav_props(dec->impl, &dec->bits_per_sample, &dec->audio_bytes);
    /* PCM: exactly the samples (AIFF's SSND chunk also counts its offset fields). */
    if (dec->bits_per_sample && dec->length > 0)
        dec->audio_bytes = (int64_t)dec->length * channels * ((dec->bits_per_sample + 7) / 8);
    snprintf(dec->codec, sizeof(dec->codec), "pcm");
    snprintf(dec->container, sizeof(dec->container), "%s", n >= 4 && memcmp(head, "FORM", 4) == 0 ? "aiff" : "wav");
    return FA_OK;
}

static fa_result wav_read(fa_decoder* dec, float* out, fa_frames frames, fa_frames* got)
{
    uint64_t n = fa_drwav_read(dec->impl, out, (uint64_t)frames);
    *got = (fa_frames)n;
    return n > 0 ? FA_OK : FA_ERROR_END_OF_STREAM;
}

static fa_result wav_seek(fa_decoder* dec, fa_frames frame)
{
    return fa_drwav_seek(dec->impl, (uint64_t)frame) ? FA_OK : FA_ERROR_NOT_SEEKABLE;
}

static void wav_close(fa_decoder* dec)
{
    fa_drwav_close(dec->impl);
    dec->impl = NULL;
}

/* ---- MP3 over the network ----------------------------------------------------
 * The length comes from the Xing/Info header, else from the file size and the
 * bitrate (counting frames would download the whole file). Seeks jump to the
 * estimated byte and restart the decoder there: within a frame or so for CBR
 * files, approximate for VBR files without a Xing header. */

typedef struct mp3_remote {
    void* dr;
    int64_t data_start;      /* first audio byte (after ID3v2 and the Xing frame) */
    int64_t data_bytes;      /* audio bytes, -1 = unknown */
    uint32_t kbps;           /* of the first frame, 0 = unknown */
} mp3_remote;

extern const fa_decoder_vtable fa_decoder_mp3_remote;

static fa_result mp3_remote_open(fa_decoder* dec)
{
    mp3_remote* m = (mp3_remote*)calloc(1, sizeof(mp3_remote));
    uint32_t rate = 0, channels = 0, probe_rate = 0;
    uint64_t frames = 0;
    const uint8_t* head;
    size_t n;
    fa_result r;
    if (!m) return FA_ERROR_OUT_OF_MEMORY;
    n = fa_source_peek(dec->src, &head);
    r = fa_drmp3_open(dec->src, &m->dr, &rate, &channels, &frames, &m->data_start);
    if (r != FA_OK) { free(m); return r; }
    if (n > 0) fa_probe_mp3_header(head, n, fa_probe_id3v2_size(head, n), &m->kbps, &probe_rate);
    m->data_bytes = dec->src->length > m->data_start ? dec->src->length - m->data_start : -1;
    dec->impl = m;
    dec->vt = &fa_decoder_mp3_remote;
    dec->format.sample_rate = rate;
    dec->format.channels = channels;
    dec->seekable = dec->src->seekable;
    if (frames > 0) dec->length = (fa_frames)frames;
    else if (m->data_bytes > 0 && m->kbps > 0)
        dec->length = (fa_frames)((double)m->data_bytes * 8.0 / (m->kbps * 1000.0) * rate);
    return FA_OK;
}

static fa_result mp3_remote_read(fa_decoder* dec, float* out, fa_frames frames, fa_frames* got)
{
    uint64_t k = fa_drmp3_read(((mp3_remote*)dec->impl)->dr, out, (uint64_t)frames);
    *got = (fa_frames)k;
    return k > 0 ? FA_OK : FA_ERROR_END_OF_STREAM;
}

static fa_result mp3_remote_seek(fa_decoder* dec, fa_frames frame)
{
    mp3_remote* m = (mp3_remote*)dec->impl;
    int64_t byte = 0;
    fa_result r;
    if (frame > 0) {
        if (dec->length > 0 && m->data_bytes > 0) byte = m->data_start + (int64_t)((double)frame / dec->length * m->data_bytes);
        else if (m->kbps > 0) byte = m->data_start + (int64_t)((double)frame * m->kbps * 125.0 / dec->format.sample_rate);
        else return FA_ERROR_NOT_SEEKABLE;
    }
    r = fa_source_seek(dec->src, byte);
    if (r != FA_OK) return r;
    return fa_drmp3_restart(m->dr);
}

static void mp3_remote_close(fa_decoder* dec)
{
    mp3_remote* m = (mp3_remote*)dec->impl;
    if (m) { fa_drmp3_close(m->dr); free(m); }
    dec->impl = NULL;
}

static void mp3_remote_lag(fa_decoder* dec, int64_t* out_bytes, int64_t* out_frames)
{
    fa_drmp3_lag(((mp3_remote*)dec->impl)->dr, out_bytes, out_frames);
}

static fa_result flac_open(fa_decoder* dec) { return open_with(dec, ma_encoding_format_flac); }
/* dec_mp3.c decodes local files itself; network sources and free-format files
 * (no bitrate in the header) come here. */
extern const fa_decoder_vtable fa_decoder_mp3_ma;
fa_result fa_mp3_open_fallback(fa_decoder* dec)
{
    fa_result r;
    if (dec->src->remote) return mp3_remote_open(dec);
    r = open_with(dec, ma_encoding_format_mp3);
    if (r == FA_OK) dec->vt = &fa_decoder_mp3_ma;
    return r;
}

const fa_decoder_vtable fa_decoder_wav = {
    "wav", "WAV, RF64, Wave64 and AIFF (PCM, float, ADPCM)", "wav;wave;rf64;w64;bwf;aif;aiff;aifc",
    "audio/wav;audio/x-wav;audio/wave;audio/vnd.wave;audio/aiff;audio/x-aiff",
    fa_probe_wav, wav_open, wav_read, wav_seek, wav_close, 1, NULL, NULL
};

const fa_decoder_vtable fa_decoder_flac = {
    "flac", "Free Lossless Audio Codec", "flac;fla",
    "audio/flac;audio/x-flac",
    fa_probe_flac, flac_open, ma_read, ma_seek, ma_close, 1, NULL, NULL
};

/* Not in the registry: fa_mp3_open_fallback switches to these. */
const fa_decoder_vtable fa_decoder_mp3_ma = {
    "mp3", "MPEG-1/2/2.5 Layer III", "mp3",
    "audio/mpeg;audio/mp3;audio/x-mp3",
    fa_probe_mp3, fa_mp3_open_fallback, ma_read, ma_seek, ma_close, 0, NULL, NULL
};

const fa_decoder_vtable fa_decoder_mp3_remote = {
    "mp3", "MPEG-1/2/2.5 Layer III", "mp3",
    "audio/mpeg;audio/mp3;audio/x-mp3",
    fa_probe_mp3, mp3_remote_open, mp3_remote_read, mp3_remote_seek, mp3_remote_close, 0, mp3_remote_lag, NULL
};
