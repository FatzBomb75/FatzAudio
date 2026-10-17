/* WAV output: 16/24-bit PCM or 32-bit float, WAVE_FORMAT_EXTENSIBLE above two
 * channels, a LIST/INFO chunk for tags, and RF64 (EBU Tech 3306) once the file
 * passes 4 GB. A JUNK chunk right after the RIFF header holds the place of the
 * ds64 chunk, so the upgrade only rewrites the header. */
#include "encode.h"
#include <stdlib.h>
#include <string.h>

#define WAV_BLOCK 4096
#define WAV_UNKNOWN 0xFFFFFFFFu

typedef struct wav_state {
    fa_enc_format f;
    uint32_t bytes_per_sample;
    int64_t data_start;          /* first byte of the audio */
    int64_t data_bytes;
    int64_t frames;
    uint32_t seed;
    int32_t* ints;
    uint8_t* bytes;
} wav_state;

static fa_result wav_check(const fa_enc_format* f)
{
    return (f->bits == 16 || f->bits == 24 || f->bits == 32) ? FA_OK : FA_ERROR_INVALID_ARG;
}

static void wav_choose(fa_enc_format* f) { (void)f; }

static fa_result write_info(fa_enc_sink* s, const char id[4], const char* text)
{
    size_t n;
    fa_result r;
    if (!text) return FA_OK;
    n = strlen(text) + 1;                    /* zero-terminated, padded to even */
    r = fa_sink_write(s, id, 4);
    if (r == FA_OK) r = fa_sink_write_le32(s, (uint32_t)n);
    if (r == FA_OK) r = fa_sink_write(s, text, n);
    if (r == FA_OK && (n & 1)) r = fa_sink_write_u8(s, 0);
    return r;
}

static uint32_t info_size(const char* text)
{
    size_t n;
    if (!text) return 0;
    n = strlen(text) + 1;
    return (uint32_t)(8 + n + (n & 1));
}

static fa_result write_list(fa_enc_sink* s, const fa_enc_tags* t)
{
    char track[16] = "";
    const char* trk = NULL;
    uint32_t size;
    fa_result r;
    if (t->track) { snprintf(track, sizeof(track), "%u", t->track); trk = track; }
    size = 4 + info_size(t->title) + info_size(t->artist) + info_size(t->album) + info_size(t->genre) +
           info_size(t->date) + info_size(t->comment) + info_size(trk);
    if (size == 4) return FA_OK;
    r = fa_sink_write(s, "LIST", 4);
    if (r == FA_OK) r = fa_sink_write_le32(s, size);
    if (r == FA_OK) r = fa_sink_write(s, "INFO", 4);
    if (r == FA_OK) r = write_info(s, "INAM", t->title);
    if (r == FA_OK) r = write_info(s, "IART", t->artist);
    if (r == FA_OK) r = write_info(s, "IPRD", t->album);
    if (r == FA_OK) r = write_info(s, "IGNR", t->genre);
    if (r == FA_OK) r = write_info(s, "ICRD", t->date);
    if (r == FA_OK) r = write_info(s, "ICMT", t->comment);
    if (r == FA_OK) r = write_info(s, "ITRK", trk);
    return r;
}

static fa_result wav_begin(void** impl, const fa_enc_format* f, const fa_enc_tags* tags, fa_enc_sink* s)
{
    wav_state* w = (wav_state*)calloc(1, sizeof(*w));
    int ext = f->channels > 2;
    uint32_t bps, align;
    fa_result r;
    if (!w) return FA_ERROR_OUT_OF_MEMORY;
    *impl = w;
    w->f = *f;
    w->bytes_per_sample = bps = f->bits / 8;
    w->seed = 0x12345678u;
    align = bps * f->channels;
    w->ints = (int32_t*)malloc((size_t)WAV_BLOCK * f->channels * sizeof(int32_t));
    w->bytes = (uint8_t*)malloc((size_t)WAV_BLOCK * align);
    if (!w->ints || !w->bytes) return FA_ERROR_OUT_OF_MEMORY;

    r = fa_sink_write(s, "RIFF", 4);
    if (r == FA_OK) r = fa_sink_write_le32(s, WAV_UNKNOWN);
    if (r == FA_OK) r = fa_sink_write(s, "WAVE", 4);
    /* JUNK: becomes ds64 (3 x 64-bit sizes, table length) if RF64 is needed. */
    if (r == FA_OK) r = fa_sink_write(s, "JUNK", 4);
    if (r == FA_OK) r = fa_sink_write_le32(s, 28);
    if (r == FA_OK) { static const uint8_t z[28] = { 0 }; r = fa_sink_write(s, z, 28); }
    if (r == FA_OK) r = fa_sink_write(s, "fmt ", 4);
    if (r == FA_OK) r = fa_sink_write_le32(s, ext ? 40 : (f->bits == 32 ? 18 : 16));
    if (r == FA_OK) r = fa_sink_write_le16(s, ext ? 0xFFFE : (f->bits == 32 ? 3 : 1));
    if (r == FA_OK) r = fa_sink_write_le16(s, f->channels);
    if (r == FA_OK) r = fa_sink_write_le32(s, f->rate);
    if (r == FA_OK) r = fa_sink_write_le32(s, f->rate * align);
    if (r == FA_OK) r = fa_sink_write_le16(s, align);
    if (r == FA_OK) r = fa_sink_write_le16(s, f->bits);
    if (ext) {
        /* KSDATAFORMAT_SUBTYPE_PCM / _IEEE_FLOAT: {0000000x-0000-0010-8000-00aa00389b71} */
        static const uint8_t guid_tail[14] = { 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00,
                                               0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };
        if (r == FA_OK) r = fa_sink_write_le16(s, 22);
        if (r == FA_OK) r = fa_sink_write_le16(s, f->bits);
        if (r == FA_OK) r = fa_sink_write_le32(s, fa_enc_channel_mask(f->channels));
        if (r == FA_OK) r = fa_sink_write_le16(s, f->bits == 32 ? 3 : 1);
        if (r == FA_OK) r = fa_sink_write(s, guid_tail, sizeof(guid_tail));
    } else if (f->bits == 32) {
        if (r == FA_OK) r = fa_sink_write_le16(s, 0);
    }
    if (r == FA_OK) r = write_list(s, tags);
    if (r == FA_OK) r = fa_sink_write(s, "data", 4);
    if (r == FA_OK) r = fa_sink_write_le32(s, WAV_UNKNOWN);
    w->data_start = s->pos;
    return r;
}

static fa_result wav_write(void* impl, const float* frames, int64_t count, fa_enc_sink* s)
{
    wav_state* w = (wav_state*)impl;
    uint32_t ch = w->f.channels;
    while (count > 0) {
        int64_t n = count < WAV_BLOCK ? count : WAV_BLOCK, i, total = n * ch;
        uint8_t* b = w->bytes;
        fa_result r;
        if (w->f.bits == 32) {
            for (i = 0; i < total; i++) {
                union { float f; uint32_t u; } v;
                v.f = frames[i];
                b[0] = (uint8_t)v.u; b[1] = (uint8_t)(v.u >> 8); b[2] = (uint8_t)(v.u >> 16); b[3] = (uint8_t)(v.u >> 24);
                b += 4;
            }
        } else {
            fa_enc_quantize(frames, w->ints, total, w->f.bits, &w->seed);
            for (i = 0; i < total; i++) {
                int32_t x = w->ints[i];
                *b++ = (uint8_t)x;
                *b++ = (uint8_t)(x >> 8);
                if (w->f.bits == 24) *b++ = (uint8_t)(x >> 16);
            }
        }
        r = fa_sink_write(s, w->bytes, (size_t)(b - w->bytes));
        if (r != FA_OK) return r;
        w->data_bytes += b - w->bytes;
        w->frames += n;
        frames += total;
        count -= n;
    }
    return FA_OK;
}

static fa_result wav_finish(void* impl, fa_enc_sink* s)
{
    wav_state* w = (wav_state*)impl;
    int64_t riff;
    fa_result r = FA_OK;
    if (w->data_bytes & 1) r = fa_sink_write_u8(s, 0);   /* chunks are even-sized */
    if (r != FA_OK || !s->seekable) return r;
    riff = s->pos - 8;
    if (riff <= 0xFFFFFFFFll) {
        r = fa_sink_seek(s, 4);
        if (r == FA_OK) r = fa_sink_write_le32(s, (uint32_t)riff);
        if (r == FA_OK) r = fa_sink_seek(s, w->data_start - 4);
        if (r == FA_OK) r = fa_sink_write_le32(s, (uint32_t)w->data_bytes);
    } else {
        r = fa_sink_seek(s, 0);
        if (r == FA_OK) r = fa_sink_write(s, "RF64", 4);
        if (r == FA_OK) r = fa_sink_write_le32(s, WAV_UNKNOWN);
        if (r == FA_OK) r = fa_sink_seek(s, 12);
        if (r == FA_OK) r = fa_sink_write(s, "ds64", 4);
        if (r == FA_OK) r = fa_sink_write_le32(s, 28);
        if (r == FA_OK) r = fa_sink_write_le64(s, (uint64_t)riff);
        if (r == FA_OK) r = fa_sink_write_le64(s, (uint64_t)w->data_bytes);
        if (r == FA_OK) r = fa_sink_write_le64(s, (uint64_t)w->frames);
        if (r == FA_OK) r = fa_sink_write_le32(s, 0);
        /* the data chunk's size stays 0xFFFFFFFF: "see ds64" */
    }
    if (r == FA_OK) r = fa_sink_seek(s, riff + 8);   /* back to the end, like the other formats */
    return r;
}

static void wav_free(void* impl)
{
    wav_state* w = (wav_state*)impl;
    free(w->ints);
    free(w->bytes);
    free(w);
}

const fa_enc_backend fa_enc_wav = { wav_check, wav_choose, wav_begin, wav_write, wav_finish, wav_free };
