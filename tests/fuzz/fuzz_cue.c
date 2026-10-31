/* Fuzz target: audio CDs without a drive. The input is a .cue sheet, a NUL, then the
 * image bytes, written as fuzz_cue.cue and fuzz_cue.bin/.wav/.sub in the working
 * directory (the sheet names them; removed after). The image opens with fa_disc_open:
 * TOC, ids, CD-Text, then a stream of each track (up to 4), read, seeked and read
 * again, with the subcode feeding CD+G. The image bytes then also answer the SCSI
 * commands of a simulated MMC drive (the TOC, CD-Text, subcode and sector reads
 * disc_mmc.c parses), which is played the same way. */
#include "fa_internal.h"
#include "disc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static fa_engine* g_engine;

static void play(fa_disc* d)
{
    static float buf[4096 * 2];
    fa_disc_toc toc;
    char text[256];
    int32_t t, k;
    if (fa_disc_get_toc(d, &toc) != FA_OK) return;
    for (t = 0; t <= toc.track_count && t < 6; t++)
        for (k = FA_DISC_TEXT_TITLE; k <= FA_DISC_TEXT_CODE; k++)
            (void)fa_disc_get_text(d, t ? toc.first_track + t - 1 : 0, (fa_disc_text)k, text, sizeof(text));
    for (t = 0; t < toc.track_count && t < 6; t++) {
        int64_t gap;
        (void)fa_disc_get_pregap(d, toc.first_track + t, &gap);   /* the cue sheet, or the Q subcode search */
    }
    for (t = 0; t < toc.track_count && t < 4; t++) {
        fa_open_options o;
        fa_stream* s = NULL;
        fa_frames got;
        fa_cdg_info ci;
        fa_open_options_init(&o);
        o.flags = FA_OPEN_DECODE_ONLY;
        if (fa_stream_open_disc(g_engine, d, toc.first_track + t, &o, &s) != FA_OK) continue;
        for (k = 0; k < 6; k++)
            if (fa_stream_read(s, buf, 4096, &got) != FA_OK || got == 0) break;
        fa_stream_seek(s, toc.tracks[t].frames / 2);
        fa_stream_read(s, buf, 4096, &got);
        fa_stream_seek(s, toc.tracks[t].frames > 0 ? toc.tracks[t].frames - 1 : 0);
        fa_stream_read(s, buf, 4096, &got);
        fa_stream_get_cdg_info(s, &ci);
        fa_stream_close(s);
    }
}

/* ---- the simulated drive -------------------------------------------------------- */

typedef struct fake { const uint8_t* d; size_t n; } fake;

static void fill(const fake* f, size_t from, uint8_t* out, size_t len)
{
    size_t i;
    if (!f->n) { memset(out, 0, len); return; }
    for (i = 0; i < len; i++) out[i] = f->d[(from + i) % f->n];
}

static fa_result fake_exec(void* h, const uint8_t* cdb, int cdb_len, void* buf, uint32_t len, int dir,
                           fa_mmc_sense* sense, uint32_t timeout_ms)
{
    const fake* f = (const fake*)h;
    uint8_t* out = (uint8_t*)buf;
    (void)cdb_len; (void)dir; (void)timeout_ms;
    switch (cdb[0]) {
    case 0x00: return FA_OK;                       /* TEST UNIT READY */
    case 0x43: {                                   /* READ TOC: format 0 from the start, CD-Text after it */
        size_t toc = f->n >= 2 ? 2u + (((size_t)f->d[0] << 8) | f->d[1]) : 0;
        fill(f, (cdb[2] & 0x0F) == 0 ? 0 : toc, out, len);
        return FA_OK;
    }
    case 0xBE: {                                   /* READ CD: sectors from the bytes, by LBA */
        int64_t lba = ((int64_t)cdb[2] << 24) | (cdb[3] << 16) | (cdb[4] << 8) | cdb[5];
        fill(f, (size_t)(lba & 0xFFFFFF) * 2352u, out, len);
        return FA_OK;
    }
    case 0x12: case 0x42: case 0x5A:               /* INQUIRY, READ SUB-CHANNEL, MODE SENSE */
        fill(f, cdb[0] * 7u, out, len);
        return FA_OK;
    case 0xBB: case 0x1B: case 0x1E: return FA_OK;
    }
    sense->key = 5; sense->asc = 0x20; sense->ascq = 0;
    return FA_ERROR_IO;
}

static void fake_close(void* h) { (void)h; }

static void put(const char* path, const uint8_t* d, size_t n)
{
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); abort(); }
    if (n) fwrite(d, 1, n, f);
    fclose(f);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    const uint8_t* nul = (const uint8_t*)memchr(data, 0, size);
    size_t cue = nul ? (size_t)(nul - data) : size;
    const uint8_t* img = data + cue + (nul ? 1 : 0);
    size_t img_n = size - cue - (nul ? 1 : 0);
    fa_disc_options opt;
    fa_disc* d = NULL;
    const fa_disc_ops* ops;
    void* h;
    fake f;
    if (!g_engine) {
        fa_engine_config cfg;
        fa_engine_config_init(&cfg);
        if (fa_engine_create(&cfg, &g_engine) != FA_OK) abort();
    }
    fa_disc_options_init(&opt);
    opt.read_offset = (int32_t)(size % 13) - 6;
    opt.retries = 1;

    put("fuzz_cue.cue", data, cue);
    put("fuzz_cue.bin", img, img_n);
    put("fuzz_cue.wav", img, img_n);
    put("fuzz_cue.sub", img, img_n);
    if (fa_disc_open("fuzz_cue.cue", &opt, &d) == FA_OK) {
        play(d);
        fa_disc_close(d);
    }

    remove("fuzz_cue.cue"); remove("fuzz_cue.bin"); remove("fuzz_cue.wav"); remove("fuzz_cue.sub");

    f.d = img;
    f.n = img_n;
    if (fa_mmc_open(fake_exec, fake_close, &f, 7, &ops, &h) == FA_OK && fa_disc_open_ops(ops, h, &opt, &d) == FA_OK) {
        play(d);
        fa_disc_close(d);
    }
    return 0;
}
