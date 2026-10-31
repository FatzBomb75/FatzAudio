/* Drive operations as SCSI MMC commands (the command set every CD/DVD/BD drive
 * speaks). Platforms that pass commands through to the drive (Linux SG_IO,
 * Windows SPTI) supply one `exec` function; this file does the rest. */
#include "disc.h"
#include <stdlib.h>
#include <string.h>

#define MMC_TIMEOUT 20000
#define MMC_TRAY_TIMEOUT 30000

typedef struct mmc_drive {
    fa_mmc_exec_fn exec;
    void (*close)(void*);
    void* h;
    uint32_t max_sectors;   /* largest transfer the OS passes through in one command */
} mmc_drive;

void fa_mmc_parse_sense(const uint8_t* d, size_t len, fa_mmc_sense* out)
{
    memset(out, 0, sizeof(*out));
    if (len < 4) return;
    if ((d[0] & 0x7F) == 0x72 || (d[0] & 0x7F) == 0x73) {   /* descriptor format */
        out->key = (uint8_t)(d[1] & 0x0F);
        out->asc = d[2];
        out->ascq = d[3];
    } else if (len >= 14) {                                   /* fixed format */
        out->key = (uint8_t)(d[2] & 0x0F);
        out->asc = d[12];
        out->ascq = d[13];
    }
}

static fa_result run(mmc_drive* m, const uint8_t* cdb, int n, void* buf, uint32_t len, int dir, fa_mmc_sense* sense,
                     uint32_t timeout)
{
    memset(sense, 0, sizeof(*sense));
    return m->exec(m->h, cdb, n, buf, len, dir, sense, timeout);
}

static int no_medium(const fa_mmc_sense* s) { return s->key == 2 && s->asc == 0x3A; }
static int media_changed(const fa_mmc_sense* s) { return s->key == 6 && s->asc == 0x28; }

static fa_result mmc_inquiry(void* h, char vendor[16], char product[32], char revision[8])
{
    uint8_t cdb[6] = { 0x12, 0, 0, 0, 36, 0 }, buf[36];
    fa_mmc_sense sense;
    fa_result r;
    memset(buf, 0, sizeof(buf));
    r = run((mmc_drive*)h, cdb, 6, buf, sizeof(buf), FA_MMC_IN, &sense, MMC_TIMEOUT);
    if (r != FA_OK) return r;
    fa_disc_copy_trimmed(vendor, 16, buf + 8, 8);
    fa_disc_copy_trimmed(product, 32, buf + 16, 16);
    fa_disc_copy_trimmed(revision, 8, buf + 32, 4);
    return FA_OK;
}

static fa_result mmc_test_ready(void* h, int* out_state, int* out_changed)
{
    uint8_t cdb[6] = { 0 };
    fa_mmc_sense sense;
    int attempt;
    *out_changed = 0;
    for (attempt = 0; attempt < 3; attempt++) {
        fa_result r = run((mmc_drive*)h, cdb, 6, NULL, 0, FA_MMC_NONE, &sense, MMC_TIMEOUT);
        if (r == FA_OK) { *out_state = FA_CD_READY; return FA_OK; }
        if (r != FA_ERROR_IO) return r;
        if (sense.key == 6) {           /* unit attention: report it and ask again */
            if (media_changed(&sense)) *out_changed = 1;
            continue;
        }
        if (no_medium(&sense)) { *out_state = sense.ascq == 0x02 ? FA_CD_TRAY_OPEN : FA_CD_NO_DISC; return FA_OK; }
        if (sense.key == 2 && sense.asc == 0x04) { *out_state = FA_CD_SPINNING_UP; return FA_OK; }
        *out_state = FA_CD_NO_DISC;     /* not ready for another reason (unreadable disc, ...) */
        return FA_OK;
    }
    *out_state = FA_CD_SPINNING_UP;
    return FA_OK;
}

static fa_result mmc_read_toc(void* h, int format, int track, uint8_t* buf, uint32_t cap, uint32_t* out_len)
{
    uint8_t cdb[10] = { 0x43, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    fa_mmc_sense sense;
    fa_result r;
    uint32_t alloc = cap > 0xFFFF ? 0xFFFF : cap, len;
    *out_len = 0;
    if (cap < 4) return FA_ERROR_INVALID_ARG;
    cdb[2] = (uint8_t)(format & 0x0F);
    cdb[6] = (uint8_t)track;
    cdb[7] = (uint8_t)(alloc >> 8);
    cdb[8] = (uint8_t)alloc;
    memset(buf, 0, alloc);
    r = run((mmc_drive*)h, cdb, 10, buf, alloc, FA_MMC_IN, &sense, MMC_TIMEOUT);
    if (r == FA_ERROR_IO && (no_medium(&sense) || media_changed(&sense))) return FA_ERROR_NO_DISC;
    if (r != FA_OK) return r;
    len = 2u + (((uint32_t)buf[0] << 8) | buf[1]);
    if (len > alloc) len = alloc;
    *out_len = len;
    return FA_OK;
}

static fa_result mmc_read_cd_once(mmc_drive* m, int64_t lba, uint32_t count, uint32_t areas, uint8_t* buf)
{
    uint8_t cdb[12] = { 0xBE, 0x04 /* expected sector type: CD-DA */, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    fa_mmc_sense sense;
    fa_result r;
    uint32_t stride = FA_CD_SECTOR_BYTES + ((areas & FA_CD_AREA_C2) ? FA_CD_C2_BYTES : 0) +
                      ((areas & FA_CD_AREA_SUB) ? FA_CD_SUB_BYTES : 0);
    cdb[2] = (uint8_t)(lba >> 24); cdb[3] = (uint8_t)(lba >> 16); cdb[4] = (uint8_t)(lba >> 8); cdb[5] = (uint8_t)lba;
    cdb[6] = (uint8_t)(count >> 16); cdb[7] = (uint8_t)(count >> 8); cdb[8] = (uint8_t)count;
    cdb[9] = (uint8_t)(0x10 | ((areas & FA_CD_AREA_C2) ? 0x02 : 0));   /* user data (+ C2 error bits) */
    cdb[10] = (uint8_t)((areas & FA_CD_AREA_SUB) ? 0x01 : 0);           /* raw P-W subcode */
    r = run(m, cdb, 12, buf, count * stride, FA_MMC_IN, &sense, MMC_TIMEOUT);
    if (r != FA_ERROR_IO) return r;
    if (no_medium(&sense) || media_changed(&sense)) return FA_ERROR_NO_DISC;
    /* Invalid opcode or field: the drive can't return C2 or subcode. Other ILLEGAL
     * REQUESTs (LBA out of range, a data sector read as audio) are read errors. */
    if (sense.key == 5 && (sense.asc == 0x20 || sense.asc == 0x24) && (areas & (FA_CD_AREA_C2 | FA_CD_AREA_SUB)))
        return FA_ERROR_NOT_IMPLEMENTED;
    return FA_ERROR_IO;
}

static fa_result mmc_read_cd(void* h, int64_t lba, uint32_t count, uint32_t areas, uint8_t* buf)
{
    mmc_drive* m = (mmc_drive*)h;
    uint32_t stride = FA_CD_SECTOR_BYTES + ((areas & FA_CD_AREA_C2) ? FA_CD_C2_BYTES : 0) +
                      ((areas & FA_CD_AREA_SUB) ? FA_CD_SUB_BYTES : 0);
    while (count > 0) {
        uint32_t n = count < m->max_sectors ? count : m->max_sectors;
        fa_result r = mmc_read_cd_once(m, lba, n, areas, buf);
        if (r != FA_OK) return r;
        lba += n;
        count -= n;
        buf += (size_t)n * stride;
    }
    return FA_OK;
}

static fa_result mmc_read_code(void* h, int which, int track, char* out, size_t cap)
{
    uint8_t cdb[10] = { 0x42, 0, 0x40 /* SubQ */, 0, 0, 0, 0, 0, 24, 0 }, buf[24];
    fa_mmc_sense sense;
    fa_result r;
    cdb[3] = (uint8_t)which;
    cdb[6] = (uint8_t)(which == FA_CD_CODE_ISRC ? track : 0);
    memset(buf, 0, sizeof(buf));
    out[0] = 0;
    r = run((mmc_drive*)h, cdb, 10, buf, sizeof(buf), FA_MMC_IN, &sense, MMC_TIMEOUT);
    if (r == FA_ERROR_IO && (no_medium(&sense) || media_changed(&sense))) return FA_ERROR_NO_DISC;
    if (r != FA_OK) return r;
    if (!(buf[8] & 0x80)) return FA_ERROR_NOT_FOUND;
    fa_disc_copy_trimmed(out, cap, buf + 9, which == FA_CD_CODE_MCN ? 13 : 12);
    if (which == FA_CD_CODE_MCN) {
        /* An all-zero catalog number means "none". */
        size_t i;
        for (i = 0; out[i] == '0'; i++) {}
        if (out[i] == 0) { out[0] = 0; return FA_ERROR_NOT_FOUND; }
    }
    return out[0] ? FA_OK : FA_ERROR_NOT_FOUND;
}

static fa_result mmc_set_speed(void* h, uint32_t kbps)
{
    uint8_t cdb[12] = { 0xBB, 0, 0, 0, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0 };
    fa_mmc_sense sense;
    if (kbps > 0xFFFF) kbps = 0xFFFF;
    cdb[2] = (uint8_t)(kbps >> 8);
    cdb[3] = (uint8_t)kbps;
    return run((mmc_drive*)h, cdb, 12, NULL, 0, FA_MMC_NONE, &sense, MMC_TIMEOUT);
}

static fa_result mmc_get_speed(void* h, uint32_t* out_kbps)
{
    /* MODE SENSE(10), page 2Ah (capabilities): current read speed at offset 14. */
    uint8_t cdb[10] = { 0x5A, 0x08, 0x2A, 0, 0, 0, 0, 0, 64, 0 }, buf[64];
    fa_mmc_sense sense;
    fa_result r;
    uint32_t bd, page;
    memset(buf, 0, sizeof(buf));
    r = run((mmc_drive*)h, cdb, 10, buf, sizeof(buf), FA_MMC_IN, &sense, MMC_TIMEOUT);
    if (r != FA_OK) return r;
    bd = ((uint32_t)buf[6] << 8) | buf[7];
    page = 8 + bd;
    if (page + 16 > sizeof(buf) || (buf[page] & 0x3F) != 0x2A) return FA_ERROR_NOT_IMPLEMENTED;
    *out_kbps = ((uint32_t)buf[page + 14] << 8) | buf[page + 15];
    return *out_kbps ? FA_OK : FA_ERROR_NOT_IMPLEMENTED;
}

static fa_result mmc_tray(void* h, int open)
{
    uint8_t cdb[6] = { 0x1B, 0, 0, 0, 0, 0 };
    fa_mmc_sense sense;
    fa_result r;
    cdb[4] = (uint8_t)(open ? 0x02 : 0x03);   /* LoEj, Start */
    r = run((mmc_drive*)h, cdb, 6, NULL, 0, FA_MMC_NONE, &sense, MMC_TRAY_TIMEOUT);
    if (r == FA_ERROR_IO && sense.key == 5) return FA_ERROR_NOT_IMPLEMENTED;   /* slot loader: cannot close */
    return r == FA_ERROR_IO ? FA_ERROR_DEVICE : r;
}

static fa_result mmc_lock(void* h, int locked)
{
    uint8_t cdb[6] = { 0x1E, 0, 0, 0, 0, 0 };
    fa_mmc_sense sense;
    fa_result r;
    cdb[4] = (uint8_t)(locked ? 1 : 0);
    r = run((mmc_drive*)h, cdb, 6, NULL, 0, FA_MMC_NONE, &sense, MMC_TIMEOUT);
    return r == FA_ERROR_IO ? FA_ERROR_DEVICE : r;
}

static void mmc_close(void* h)
{
    mmc_drive* m = (mmc_drive*)h;
    if (m->close) m->close(m->h);
    free(m);
}

static const fa_disc_ops mmc_ops = {
    mmc_inquiry, mmc_test_ready, mmc_read_toc, mmc_read_cd, mmc_read_code,
    mmc_set_speed, mmc_get_speed, mmc_tray, mmc_lock, mmc_close, NULL
};

fa_result fa_mmc_open(fa_mmc_exec_fn exec, void (*close)(void*), void* h, uint32_t max_sectors,
                      const fa_disc_ops** out_ops, void** out_handle)
{
    mmc_drive* m = (mmc_drive*)calloc(1, sizeof(*m));
    if (!m) { if (close) close(h); return FA_ERROR_OUT_OF_MEMORY; }
    m->exec = exec;
    m->close = close;
    m->h = h;
    m->max_sectors = max_sectors ? max_sectors : 16;
    *out_ops = &mmc_ops;
    *out_handle = m;
    return FA_OK;
}
