/* miniaudio implementation lives in its own translation unit. miniaudio embeds
 * dr_wav / dr_mp3 / dr_flac, so WAV, MP3 and FLAC decoding come from it. */
#define MINIAUDIO_IMPLEMENTATION
/* minimp3 synthesizes in float: keep it, rather than rounding to 16 bits. */
#define MA_DR_MP3_FLOAT_OUTPUT
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

/* Little-endian 16, 24 and 32-bit integer or 32-bit float PCM on a little-endian
 * host: the bytes are read in one go straight into the back of `out` and widened
 * to float in place, front to back. dr_wav's own path reads 4 KB at a time (a
 * system call each, on Windows) and widens 24-bit samples through double. */
static uint64_t read_pcm_direct(ma_dr_wav* w, float* out, uint64_t frames, uint32_t bytes)
{
    enum { BLOCK = 512 };
    size_t n, i, j, c;
    uint64_t got;
    uint8_t* raw = (uint8_t*)out + (size_t)frames * w->channels * (4 - bytes);
    got = ma_dr_wav_read_pcm_frames(w, frames, raw);
    n = (size_t)got * w->channels;
    if (w->translatedFormatTag == MA_DR_WAVE_FORMAT_IEEE_FLOAT) return got;   /* already there */
    /* A block is copied out before its floats are written: they end where the
     * raw bytes still to read begin, at the earliest. */
    for (i = 0; i < n; i += c) {
        c = n - i < BLOCK ? n - i : BLOCK;
        if (bytes == 2) {
            ma_int16 x[BLOCK];
            memcpy(x, raw + 2 * i, 2 * c);
            ma_dr_wav_s16_to_f32(out + i, x, c);
        } else if (bytes == 3) {
            uint8_t x[3 * BLOCK];
            memcpy(x, raw + 3 * i, 3 * c);
            for (j = 0; j < c; j++) {
                int32_t v = (int32_t)(((uint32_t)x[3 * j] << 8) | ((uint32_t)x[3 * j + 1] << 16) | ((uint32_t)x[3 * j + 2] << 24)) >> 8;
                out[i + j] = (float)v * (1.0f / 8388608.0f);
            }
        } else {
            ma_int32 x[BLOCK];
            memcpy(x, raw + 4 * i, 4 * c);
            ma_dr_wav_s32_to_f32(out + i, x, c);
        }
    }
    return got;
}

void fa_drwav_props(void* impl, uint32_t* out_bits, int64_t* out_data_bytes)
{
    ma_dr_wav* w = (ma_dr_wav*)impl;
    int pcm = w->translatedFormatTag == MA_DR_WAVE_FORMAT_PCM || w->translatedFormatTag == MA_DR_WAVE_FORMAT_IEEE_FLOAT;
    *out_bits = pcm ? w->bitsPerSample : 0;
    *out_data_bytes = w->dataChunkDataSize > 0 ? (int64_t)w->dataChunkDataSize : -1;
}

uint64_t fa_drwav_read(void* impl, float* out, uint64_t frames)
{
    ma_dr_wav* w = (ma_dr_wav*)impl;
    uint32_t bpf = ma_dr_wav_get_bytes_per_pcm_frame(w), bytes = w->channels ? bpf / w->channels : 0;
    int le = w->container == ma_dr_wav_container_riff || w->container == ma_dr_wav_container_rf64 ||
             w->container == ma_dr_wav_container_w64;
    int fmt_ok = (w->translatedFormatTag == MA_DR_WAVE_FORMAT_PCM && bytes >= 2 && bytes <= 4) ||
                 (w->translatedFormatTag == MA_DR_WAVE_FORMAT_IEEE_FLOAT && bytes == 4);
    uint64_t got;
    if (ma_dr_wav__is_little_endian() && le && fmt_ok && bytes * w->channels == bpf)
        got = read_pcm_direct(w, out, frames, bytes);
    else
        got = ma_dr_wav_read_pcm_frames_f32(w, frames, out);
    /* Float files can hold NaN or infinity; integer and ADPCM data can't. */
    if (w->translatedFormatTag == MA_DR_WAVE_FORMAT_IEEE_FLOAT) fa_zero_non_finite(out, (size_t)got * w->channels);
    return got;
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

/* ---- minimp3 frame decoder for packet streams (codec.c) --------------------- */

void* fa_mp3dec_create(void)
{
    ma_dr_mp3dec* d = (ma_dr_mp3dec*)calloc(1, sizeof(ma_dr_mp3dec));
    if (d) ma_dr_mp3dec_init(d);
    return d;
}

void fa_mp3dec_reset(void* dec)
{
    ma_dr_mp3dec_init((ma_dr_mp3dec*)dec);
}

int fa_mp3dec_decode(void* dec, const uint8_t* data, int size, float* pcm, int* out_frames,
                     int* out_channels, int* out_rate)
{
    ma_dr_mp3dec_frame_info info;
    int frames;
    memset(&info, 0, sizeof(info));
    frames = ma_dr_mp3dec_decode_frame((ma_dr_mp3dec*)dec, data, size, pcm, &info);
    *out_frames = frames;
    *out_channels = info.channels;
    *out_rate = info.sample_rate;
    return info.frame_bytes;
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
    if (!ma_dr_mp3_init(&m->dr, mp3_on_read, mp3_on_seek, NULL, NULL, m, NULL)) return 0;
    /* Without a LAME header dr_mp3 keeps the filter bank's 529-sample delay;
     * drop it as local files do (dec_mp3.c), so a web stream and the same file
     * on disk start on the same sample. */
    if (m->dr.delayInPCMFrames == 0) m->dr.delayInPCMFrames = 529;
    return 1;
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

/* ---- dr_flac glue for codec_flac.c ------------------------------------------ */

/* dr_flac allocates its decoder and one block's samples in one go: the first
 * allocation gets the cached block, anything else the heap. */
static void* flac_cache_malloc(size_t sz, void* user)
{
    fa_drflac_cache* c = (fa_drflac_cache*)user;
    if (c->used) return malloc(sz);
    if (sz > c->cap) {
        void* p = realloc(c->block, sz);
        if (!p) return NULL;
        c->block = p;
        c->cap = sz;
    }
    c->used = 1;
    return c->block;
}

static void* flac_cache_realloc(void* p, size_t sz, void* user)
{
    fa_drflac_cache* c = (fa_drflac_cache*)user;
    if (!p) return flac_cache_malloc(sz, user);
    if (p == c->block) {
        if (sz > c->cap) {
            void* q = realloc(c->block, sz);
            if (!q) return NULL;
            c->block = q;
            c->cap = sz;
        }
        return c->block;
    }
    return realloc(p, sz);
}

static void flac_cache_free(void* p, void* user)
{
    fa_drflac_cache* c = (fa_drflac_cache*)user;
    if (p && p == c->block) c->used = 0;
    else free(p);
}

int64_t fa_drflac_decode_memory(const void* data, size_t size, float* out, uint64_t max_frames,
                                uint32_t* out_channels, uint32_t* out_rate, fa_drflac_cache* cache)
{
    ma_allocation_callbacks alloc;
    ma_dr_flac* f;
    int64_t n;
    alloc.pUserData = cache;
    alloc.onMalloc = flac_cache_malloc;
    alloc.onRealloc = flac_cache_realloc;
    alloc.onFree = flac_cache_free;
    f = ma_dr_flac_open_memory(data, size, cache ? &alloc : NULL);
    if (!f) return -1;
    *out_channels = f->channels;
    *out_rate = f->sampleRate;
    n = f->channels == 0 ? -1 : (int64_t)ma_dr_flac_read_pcm_frames_f32(f, max_frames, out);
    ma_dr_flac_close(f);
    if (cache) cache->used = 0;   /* nothing of this decode lives on, whatever got the block */
    return n;
}
