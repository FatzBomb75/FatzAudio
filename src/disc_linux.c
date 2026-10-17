/* CD drives on Linux: /dev/sr* with MMC commands through SG_IO. Android has no
 * optical drives, so it lists none (disc images still work there). */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "disc.h"
#include <stdio.h>
#include <string.h>

#if defined(__linux__) && !defined(__ANDROID__)

#include <errno.h>
#include <fcntl.h>
#include <scsi/sg.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define LINUX_MAX_SECTORS 23   /* keeps every transfer under 64 KB (some USB bridges) */

typedef struct linux_drive { int fd; } linux_drive;

static fa_result linux_exec(void* h, const uint8_t* cdb, int cdb_len, void* buf, uint32_t len, int dir,
                            fa_mmc_sense* sense, uint32_t timeout_ms)
{
    linux_drive* d = (linux_drive*)h;
    sg_io_hdr_t io;
    uint8_t sb[64];
    memset(&io, 0, sizeof(io));
    memset(sb, 0, sizeof(sb));
    io.interface_id = 'S';
    io.cmd_len = (unsigned char)cdb_len;
    io.cmdp = (unsigned char*)cdb;
    io.dxfer_direction = dir == FA_MMC_IN ? SG_DXFER_FROM_DEV : dir == FA_MMC_OUT ? SG_DXFER_TO_DEV : SG_DXFER_NONE;
    io.dxferp = len ? buf : NULL;
    io.dxfer_len = len;
    io.sbp = sb;
    io.mx_sb_len = sizeof(sb);
    io.timeout = timeout_ms;
    if (ioctl(d->fd, SG_IO, &io) < 0) return errno == ENOMEDIUM ? FA_ERROR_NO_DISC : FA_ERROR_DEVICE;
    if ((io.info & SG_INFO_OK_MASK) == SG_INFO_OK) return FA_OK;
    if (io.sb_len_wr > 0) {
        fa_mmc_parse_sense(sb, io.sb_len_wr, sense);
        return FA_ERROR_IO;
    }
    return FA_ERROR_DEVICE;
}

static void linux_close(void* h)
{
    linux_drive* d = (linux_drive*)h;
    close(d->fd);
    free(d);
}

uint32_t fa_disc_platform_list(char (*ids)[256], uint32_t cap)
{
    uint32_t n = 0;
    int i;
    for (i = 0; i < 32; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/sr%d", i);
        if (access(path, F_OK) != 0) continue;
        if (n < cap) snprintf(ids[n], 256, "%s", path);
        n++;
    }
    return n;
}

fa_result fa_disc_platform_open(const char* id, const fa_disc_ops** out_ops, void** out_handle)
{
    linux_drive* d;
    uint8_t cdb[6] = { 0x12, 0, 0, 0, 36, 0 }, inq[36];
    fa_mmc_sense sense;
    int fd = open(id, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0 && (errno == EACCES || errno == EPERM || errno == EROFS)) fd = open(id, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return (errno == ENOENT || errno == ENXIO || errno == ENODEV) ? FA_ERROR_NOT_FOUND : FA_ERROR_DEVICE;
    d = (linux_drive*)malloc(sizeof(*d));
    if (!d) { close(fd); return FA_ERROR_OUT_OF_MEMORY; }
    d->fd = fd;
    /* Only optical drives (peripheral device type 5), never a hard disk by mistake. */
    memset(inq, 0, sizeof(inq));
    if (linux_exec(d, cdb, 6, inq, sizeof(inq), FA_MMC_IN, &sense, 5000) != FA_OK || (inq[0] & 0x1F) != 5) {
        linux_close(d);
        return FA_ERROR_NOT_FOUND;
    }
    return fa_mmc_open(linux_exec, linux_close, d, LINUX_MAX_SECTORS, out_ops, out_handle);
}

#elif !defined(_WIN32) && !defined(__APPLE__)

uint32_t fa_disc_platform_list(char (*ids)[256], uint32_t cap) { (void)ids; (void)cap; return 0; }

fa_result fa_disc_platform_open(const char* id, const fa_disc_ops** out_ops, void** out_handle)
{
    (void)id; (void)out_ops; (void)out_handle;
    return FA_ERROR_NOT_FOUND;
}

#endif
