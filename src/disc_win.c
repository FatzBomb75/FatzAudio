/* CD drives on Windows: \\.\D: with MMC commands through IOCTL_SCSI_PASS_THROUGH.
 * Works without administrator rights. */
#include "disc.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>
#include <ntddscsi.h>

#define WIN_MAX_SECTORS 23   /* keeps every transfer under 64 KB */

typedef struct win_drive { HANDLE h; } win_drive;

typedef struct spt_buffer {
    SCSI_PASS_THROUGH spt;
    ULONG filler;            /* aligns the sense buffer */
    UCHAR sense[32];
    UCHAR data[1];
} spt_buffer;

static fa_result win_exec(void* h, const uint8_t* cdb, int cdb_len, void* buf, uint32_t len, int dir,
                          fa_mmc_sense* sense, uint32_t timeout_ms)
{
    win_drive* d = (win_drive*)h;
    size_t total = offsetof(spt_buffer, data) + len;
    spt_buffer* b = (spt_buffer*)calloc(1, total);
    DWORD ret = 0;
    fa_result r = FA_OK;
    if (!b) return FA_ERROR_OUT_OF_MEMORY;
    b->spt.Length = sizeof(SCSI_PASS_THROUGH);
    b->spt.CdbLength = (UCHAR)cdb_len;
    b->spt.SenseInfoLength = sizeof(b->sense);
    b->spt.DataIn = dir == FA_MMC_IN ? SCSI_IOCTL_DATA_IN : dir == FA_MMC_OUT ? SCSI_IOCTL_DATA_OUT : SCSI_IOCTL_DATA_UNSPECIFIED;
    b->spt.DataTransferLength = len;
    b->spt.TimeOutValue = (timeout_ms + 999) / 1000;
    b->spt.DataBufferOffset = offsetof(spt_buffer, data);
    b->spt.SenseInfoOffset = (ULONG)offsetof(spt_buffer, sense);
    memcpy(b->spt.Cdb, cdb, (size_t)cdb_len);
    if (dir == FA_MMC_OUT && len) memcpy(b->data, buf, len);
    if (!DeviceIoControl(d->h, IOCTL_SCSI_PASS_THROUGH, b, (DWORD)total, b, (DWORD)total, &ret, NULL)) {
        DWORD e = GetLastError();
        r = (e == ERROR_NOT_READY || e == ERROR_MEDIA_CHANGED) ? FA_ERROR_NO_DISC : FA_ERROR_DEVICE;
    } else if (b->spt.ScsiStatus != 0) {
        fa_mmc_parse_sense(b->sense, b->spt.SenseInfoLength, sense);
        r = FA_ERROR_IO;
    } else if (dir == FA_MMC_IN && len) {
        memcpy(buf, b->data, len);
    }
    free(b);
    return r;
}

static void win_close(void* h)
{
    win_drive* d = (win_drive*)h;
    CloseHandle(d->h);
    free(d);
}

uint32_t fa_disc_platform_list(char (*ids)[256], uint32_t cap)
{
    DWORD mask = GetLogicalDrives();
    uint32_t n = 0;
    int i;
    for (i = 0; i < 26; i++) {
        char root[4] = { (char)('A' + i), ':', '\\', 0 };
        if (!(mask & (1u << i)) || GetDriveTypeA(root) != DRIVE_CDROM) continue;
        if (n < cap) snprintf(ids[n], 256, "%c:", 'A' + i);
        n++;
    }
    return n;
}

fa_result fa_disc_platform_open(const char* id, const fa_disc_ops** out_ops, void** out_handle)
{
    char letter;
    WCHAR path[8], root[4];
    HANDLE h;
    win_drive* d;
    /* "D:", "D:\", "d" or "\\.\D:" */
    if (id[0] == '\\' && id[1] == '\\' && id[2] == '.' && id[3] == '\\') id += 4;
    letter = id[0];
    if (letter >= 'a' && letter <= 'z') letter = (char)(letter - 'a' + 'A');
    if (letter < 'A' || letter > 'Z' || (id[1] && (id[1] != ':' || (id[2] && (id[2] != '\\' || id[3])))))
        return FA_ERROR_NOT_FOUND;
    root[0] = (WCHAR)letter; root[1] = L':'; root[2] = L'\\'; root[3] = 0;
    if (GetDriveTypeW(root) != DRIVE_CDROM) return FA_ERROR_NOT_FOUND;
    wcscpy(path, L"\\\\.\\");
    path[4] = (WCHAR)letter; path[5] = L':'; path[6] = 0;
    h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return FA_ERROR_DEVICE;
    d = (win_drive*)malloc(sizeof(*d));
    if (!d) { CloseHandle(h); return FA_ERROR_OUT_OF_MEMORY; }
    d->h = h;
    return fa_mmc_open(win_exec, win_close, d, WIN_MAX_SECTORS, out_ops, out_handle);
}
