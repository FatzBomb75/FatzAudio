/* SF3 soundfonts (MuseScore's format, also written by Polyphone): a SoundFont 2
 * whose samples are Ogg Vorbis streams instead of 16-bit PCM. Turned into a
 * plain SoundFont 2 image in memory before loading, as DLS banks are, so the
 * synthesizer only ever sees SF2.
 *
 * In SF3 a sample header's start and end are byte offsets of its Ogg stream in
 * the smpl chunk, its loop points are relative to the sample's own start, and
 * sampleType has the "Vorbis" flag (0x10; 0x20 and 0x30 are the other codecs of
 * that flag field, never used). Each compressed sample is decoded with the
 * library's own Ogg Vorbis decoder and written as 16-bit samples followed by the
 * 46 zero samples SF2 asks for; uncompressed samples in the same bank are copied.
 * Everything else in the file (INFO, every pdta chunk but shdr) is copied as is. */
#include "fa_internal.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SF2_PAD 46                          /* zero samples after each sample */
#define SHDR_SIZE 46
#define SF3_CODEC_MASK 0x30                 /* sampleType bits: compressed */
#define SF3_ROM 0x8000
#define SF3_MAX_SAMPLE_FRAMES (1u << 26)    /* about 25 minutes at 44.1 kHz */

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void wr16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

typedef struct obuf { uint8_t* d; size_t n, cap; int oom; } obuf;

static uint8_t* ob_room(obuf* o, size_t n)
{
    if (o->oom) return NULL;
    if (n > o->cap - o->n || !o->d) {
        size_t cap = o->cap ? o->cap : 65536;
        uint8_t* grown;
        while (cap - o->n < n) {
            if (cap > ((size_t)1 << 31)) { o->oom = 1; return NULL; }
            cap *= 2;
        }
        grown = (uint8_t*)realloc(o->d, cap);
        if (!grown) { o->oom = 1; return NULL; }
        o->d = grown;
        o->cap = cap;
    }
    o->n += n;
    return o->d + o->n - n;
}

static void ob_put(obuf* o, const void* p, size_t n)
{
    uint8_t* at = ob_room(o, n);
    if (!at) return;
    if (p) memcpy(at, p, n);
    else memset(at, 0, n);
}

/* Starts a chunk (id, size filled in by ob_end); returns where it starts. */
static size_t ob_begin(obuf* o, const char* id, const char* list_type)
{
    size_t at = o->n;
    ob_put(o, id, 4);
    ob_put(o, NULL, 4);
    if (list_type) ob_put(o, list_type, 4);
    return at;
}

static void ob_end(obuf* o, size_t at)
{
    if (o->oom) return;
    wr32(o->d + at + 4, (uint32_t)(o->n - at - 8));
    if ((o->n - at) & 1) ob_put(o, NULL, 1);
}

/* The chunks of a RIFF body: calls back with each (id, data, size); sizes are
 * clamped to what is there. */
typedef struct riff_it { const uint8_t* p; size_t n, pos; } riff_it;

static int riff_next(riff_it* it, const uint8_t** id, const uint8_t** data, uint32_t* size)
{
    uint32_t sz;
    if (it->pos + 8 > it->n) return 0;
    *id = it->p + it->pos;
    sz = rd32(it->p + it->pos + 4);
    if (sz > it->n - it->pos - 8) sz = (uint32_t)(it->n - it->pos - 8);
    *data = it->p + it->pos + 8;
    *size = sz;
    it->pos += 8 + (size_t)sz + (sz & 1);
    return 1;
}

typedef struct sf3_parts {
    const uint8_t* info; uint32_t info_size;     /* LIST INFO body after its type */
    const uint8_t* smpl; uint32_t smpl_size;
    const uint8_t* pdta; uint32_t pdta_size;     /* LIST pdta body after its type */
    const uint8_t* shdr; uint32_t shdr_size;
} sf3_parts;

static int find_parts(const uint8_t* d, size_t n, sf3_parts* p)
{
    riff_it top, sub;
    const uint8_t *id, *data, *sid, *sdata;
    uint32_t size, ssize;
    memset(p, 0, sizeof(*p));
    if (n < 12 || memcmp(d, "RIFF", 4) != 0 || memcmp(d + 8, "sfbk", 4) != 0) return 0;
    top.p = d + 12;
    top.n = n - 12;
    if (rd32(d + 4) >= 4 && (size_t)rd32(d + 4) - 4 < top.n) top.n = (size_t)rd32(d + 4) - 4;
    top.pos = 0;
    while (riff_next(&top, &id, &data, &size)) {
        if (memcmp(id, "LIST", 4) != 0 || size < 4) continue;
        if (memcmp(data, "INFO", 4) == 0) { p->info = data + 4; p->info_size = size - 4; }
        if (memcmp(data, "sdta", 4) == 0) {
            sub.p = data + 4; sub.n = size - 4; sub.pos = 0;
            while (riff_next(&sub, &sid, &sdata, &ssize))
                if (memcmp(sid, "smpl", 4) == 0 && !p->smpl) { p->smpl = sdata; p->smpl_size = ssize; }
        }
        if (memcmp(data, "pdta", 4) == 0) {
            p->pdta = data + 4; p->pdta_size = size - 4;
            sub.p = p->pdta; sub.n = p->pdta_size; sub.pos = 0;
            while (riff_next(&sub, &sid, &sdata, &ssize))
                if (memcmp(sid, "shdr", 4) == 0 && !p->shdr) { p->shdr = sdata; p->shdr_size = ssize; }
        }
    }
    return p->pdta && p->shdr && p->shdr_size >= SHDR_SIZE;
}

int fa_sf3_probe(const uint8_t* d, size_t n)
{
    sf3_parts p;
    uint32_t i, count;
    if (!find_parts(d, n, &p)) return 0;
    count = p.shdr_size / SHDR_SIZE;
    for (i = 0; i + 1 < count; i++)   /* the last record is the terminal EOS */
        if (rd16(p.shdr + (size_t)i * SHDR_SIZE + 44) & SF3_CODEC_MASK) return 1;
    return 0;
}

/* One Ogg Vorbis sample, its first channel as 16-bit samples appended to `out`.
 * Returns the frames written; 0 if it can't be decoded. */
static uint32_t decode_vorbis(const uint8_t* p, size_t n, obuf* out)
{
    fa_source src;
    fa_decoder dec;
    float buf[4096];
    uint32_t frames = 0;
    if (fa_source_open_memory_ref(&src, p, n) != FA_OK) return 0;
    memset(&dec, 0, sizeof(dec));
    if (fa_decoder_open(&dec, &src, NULL, NULL, -1, NULL) != FA_OK) { fa_source_close(&src); return 0; }
    if (strcmp(dec.codec, "vorbis") == 0 && dec.format.channels >= 1 && dec.format.channels <= 8) {
        uint32_t ch = dec.format.channels;
        fa_frames want = (fa_frames)(sizeof(buf) / sizeof(buf[0]) / ch), got = 0, i;
        while (frames < SF3_MAX_SAMPLE_FRAMES && fa_decoder_read(&dec, buf, want, &got) == FA_OK && got > 0) {
            uint8_t* at = ob_room(out, (size_t)got * 2);
            if (!at) break;
            for (i = 0; i < got; i++) {
                float v = buf[i * ch] * 32767.0f;
                long s = lrintf(v < -32768.0f ? -32768.0f : v > 32767.0f ? 32767.0f : v);
                wr16(at + i * 2, (uint32_t)(uint16_t)(int16_t)s);
            }
            frames += (uint32_t)got;
        }
    }
    fa_decoder_close(&dec);
    fa_source_close(&src);
    return frames;
}

fa_result fa_sf3_to_sf2(const uint8_t* d, size_t n, uint8_t** out, size_t* out_n)
{
    sf3_parts p;
    obuf o = { 0 }, smpl = { 0 }, shdr = { 0 };
    uint32_t i, count;
    size_t riff_at, list_at, chunk_at;
    riff_it sub;
    const uint8_t *sid, *sdata;
    uint32_t ssize;
    *out = NULL;
    *out_n = 0;
    if (!find_parts(d, n, &p)) return FA_ERROR_UNSUPPORTED_FORMAT;
    count = p.shdr_size / SHDR_SIZE;

    /* The samples, and their headers pointing into the new sample data. */
    for (i = 0; i < count && !smpl.oom && !shdr.oom; i++) {
        uint8_t rec[SHDR_SIZE];
        uint32_t start, end, sl, el, type, at = (uint32_t)(smpl.n / 2), frames = 0;
        memcpy(rec, p.shdr + (size_t)i * SHDR_SIZE, SHDR_SIZE);
        if (i + 1 == count) { ob_put(&shdr, rec, SHDR_SIZE); break; }   /* EOS */
        start = rd32(rec + 20); end = rd32(rec + 24); sl = rd32(rec + 28); el = rd32(rec + 32);
        type = rd16(rec + 44);
        if (type & SF3_ROM) {
            /* ROM samples (of a sound card) have no data in the file. */
        } else if (type & SF3_CODEC_MASK) {
            if (start < end && end <= p.smpl_size) frames = decode_vorbis(p.smpl + start, end - start, &smpl);
            /* Loops are relative to the sample's start in SF3. */
            sl = sl < frames ? sl : frames;
            el = el < frames ? el : frames;
            type &= ~(uint32_t)SF3_CODEC_MASK;
        } else {
            /* 16-bit PCM in the same chunk, in samples: copied with its loops. */
            uint32_t total = p.smpl_size / 2;
            if (start < end && end <= total) {
                frames = end - start;
                ob_put(&smpl, p.smpl + (size_t)start * 2, (size_t)frames * 2);
            }
            sl = sl >= start && sl - start <= frames ? sl - start : 0;
            el = el >= start && el - start <= frames ? el - start : frames;
        }
        if (frames == 0) {
            /* Nothing usable: one silent sample, so the region still ends where it starts. */
            ob_put(&smpl, NULL, 2);
            frames = 1;
            sl = 0;
            el = 1;
        }
        if ((size_t)at + frames + SF2_PAD > 0x7FFFFFFFu) { free(smpl.d); free(shdr.d); return FA_ERROR_UNSUPPORTED_FORMAT; }
        ob_put(&smpl, NULL, SF2_PAD * 2);
        wr32(rec + 20, at);
        wr32(rec + 24, at + frames);
        wr32(rec + 28, at + sl);
        wr32(rec + 32, at + el);
        wr16(rec + 44, type);
        ob_put(&shdr, rec, SHDR_SIZE);
    }
    if (smpl.oom || shdr.oom) { free(smpl.d); free(shdr.d); return FA_ERROR_OUT_OF_MEMORY; }

    /* RIFF sfbk { LIST INFO, LIST sdta { smpl }, LIST pdta { ..., shdr, ... } } */
    riff_at = ob_begin(&o, "RIFF", "sfbk");
    list_at = ob_begin(&o, "LIST", "INFO");
    if (p.info) ob_put(&o, p.info, p.info_size);
    ob_end(&o, list_at);
    list_at = ob_begin(&o, "LIST", "sdta");
    chunk_at = ob_begin(&o, "smpl", NULL);
    ob_put(&o, smpl.d, smpl.n);
    ob_end(&o, chunk_at);
    ob_end(&o, list_at);
    list_at = ob_begin(&o, "LIST", "pdta");
    sub.p = p.pdta; sub.n = p.pdta_size; sub.pos = 0;
    while (riff_next(&sub, &sid, &sdata, &ssize)) {
        char id[5];
        memcpy(id, sid, 4);
        id[4] = '\0';
        chunk_at = ob_begin(&o, id, NULL);
        if (memcmp(sid, "shdr", 4) == 0) ob_put(&o, shdr.d, shdr.n);
        else ob_put(&o, sdata, ssize);
        ob_end(&o, chunk_at);
    }
    ob_end(&o, list_at);
    ob_end(&o, riff_at);
    free(smpl.d);
    free(shdr.d);
    if (o.oom) { free(o.d); return FA_ERROR_OUT_OF_MEMORY; }
    *out = o.d;
    *out_n = o.n;
    return FA_OK;
}
