/* CD drives on macOS: /dev/rdiskN through the IOCDMedia ioctls (DKIOCCD*), which
 * pass READ TOC and READ CD through to the drive. Only drives with a disc in them
 * have a disk node, so only those are listed. iOS has no optical drives. */
#include "disc.h"
#include <stdio.h>
#include <string.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_OSX

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOBSD.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/storage/IOCDMedia.h>
#include <IOKit/storage/IOCDMediaBSDClient.h>
#include <IOKit/storage/IOCDTypes.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/disk.h>
#include <sys/ioctl.h>
#include <unistd.h>

typedef struct mac_drive {
    int fd;
    char bsd[64];
} mac_drive;

static fa_result errno_result(void)
{
    if (errno == ENXIO || errno == ENODEV || errno == EBADF) return FA_ERROR_NO_DISC;
    return FA_ERROR_IO;
}

static void cfstring_copy(CFTypeRef v, char* out, size_t cap)
{
    out[0] = 0;
    if (v && CFGetTypeID(v) == CFStringGetTypeID()) {
        CFStringGetCString((CFStringRef)v, out, (CFIndex)cap, kCFStringEncodingUTF8);
        {
            size_t n = strlen(out);
            while (n > 0 && out[n - 1] == ' ') out[--n] = 0;
        }
    }
}

static fa_result mac_inquiry(void* h, char vendor[16], char product[32], char revision[8])
{
    mac_drive* d = (mac_drive*)h;
    io_service_t svc = IOServiceGetMatchingService(MACH_PORT_NULL, IOBSDNameMatching(MACH_PORT_NULL, 0, d->bsd));
    CFTypeRef dict;
    vendor[0] = product[0] = revision[0] = 0;
    if (!svc) return FA_ERROR_NOT_FOUND;
    dict = IORegistryEntrySearchCFProperty(svc, kIOServicePlane, CFSTR("Device Characteristics"), kCFAllocatorDefault,
                                           kIORegistryIterateRecursively | kIORegistryIterateParents);
    IOObjectRelease(svc);
    if (!dict) return FA_ERROR_NOT_FOUND;
    if (CFGetTypeID(dict) == CFDictionaryGetTypeID()) {
        cfstring_copy(CFDictionaryGetValue((CFDictionaryRef)dict, CFSTR("Vendor Name")), vendor, 16);
        cfstring_copy(CFDictionaryGetValue((CFDictionaryRef)dict, CFSTR("Product Name")), product, 32);
        cfstring_copy(CFDictionaryGetValue((CFDictionaryRef)dict, CFSTR("Product Revision Level")), revision, 8);
    }
    CFRelease(dict);
    return FA_OK;
}

static fa_result mac_test_ready(void* h, int* out_state, int* out_changed)
{
    mac_drive* d = (mac_drive*)h;
    uint64_t blocks = 0;
    *out_changed = 0;
    /* The disk node goes away with the disc, so a failing ioctl means "no disc". */
    *out_state = ioctl(d->fd, DKIOCGETBLOCKCOUNT, &blocks) == 0 && blocks > 0 ? FA_CD_READY : FA_CD_NO_DISC;
    return FA_OK;
}

static fa_result mac_read_toc(void* h, int format, int track, uint8_t* buf, uint32_t cap, uint32_t* out_len)
{
    mac_drive* d = (mac_drive*)h;
    dk_cd_read_toc_t t;
    uint32_t len;
    *out_len = 0;
    if (cap > 0xFFFF) cap = 0xFFFF;
    memset(&t, 0, sizeof(t));
    memset(buf, 0, cap);
    t.format = (uint8_t)format;
    t.formatAsTime = 0;
    t.address.track = (uint8_t)track;
    t.bufferLength = (uint16_t)cap;
    t.buffer = buf;
    if (ioctl(d->fd, DKIOCCDREADTOC, &t) != 0) return errno_result();
    len = 2u + (((uint32_t)buf[0] << 8) | buf[1]);
    if (len > cap) len = cap;
    *out_len = len;
    return FA_OK;
}

static fa_result mac_read_cd(void* h, int64_t lba, uint32_t count, uint32_t areas, uint8_t* buf)
{
    mac_drive* d = (mac_drive*)h;
    dk_cd_read_t r;
    uint32_t stride = FA_CD_SECTOR_BYTES + ((areas & FA_CD_AREA_C2) ? FA_CD_C2_BYTES : 0) +
                      ((areas & FA_CD_AREA_SUB) ? FA_CD_SUB_BYTES : 0);
    memset(&r, 0, sizeof(r));
    r.offset = (uint64_t)lba * stride;   /* byte offset in units of the requested sector size */
    r.sectorArea = (uint8_t)(kCDSectorAreaUser | ((areas & FA_CD_AREA_C2) ? kCDSectorAreaErrorFlags : 0) |
                             ((areas & FA_CD_AREA_SUB) ? kCDSectorAreaSubChannel : 0));
    r.sectorType = kCDSectorTypeCDDA;
    r.bufferLength = count * stride;
    r.buffer = buf;
    if (ioctl(d->fd, DKIOCCDREAD, &r) != 0) {
        if (errno == EINVAL && (areas & (FA_CD_AREA_C2 | FA_CD_AREA_SUB))) return FA_ERROR_NOT_IMPLEMENTED;
        return errno_result();
    }
    return r.bufferLength == count * stride ? FA_OK : FA_ERROR_IO;
}

static fa_result mac_read_code(void* h, int which, int track, char* out, size_t cap)
{
    mac_drive* d = (mac_drive*)h;
    out[0] = 0;
    if (which == FA_CD_CODE_ISRC) {
        dk_cd_read_isrc_t q;
        memset(&q, 0, sizeof(q));
        q.track = (uint8_t)track;
        if (ioctl(d->fd, DKIOCCDREADISRC, &q) != 0) return FA_ERROR_NOT_FOUND;
        fa_disc_copy_trimmed(out, cap, (const uint8_t*)q.isrc, sizeof(q.isrc));
    } else {
        dk_cd_read_mcn_t q;
        memset(&q, 0, sizeof(q));
        if (ioctl(d->fd, DKIOCCDREADMCN, &q) != 0) return FA_ERROR_NOT_FOUND;
        fa_disc_copy_trimmed(out, cap, (const uint8_t*)q.mcn, sizeof(q.mcn));
    }
    return out[0] ? FA_OK : FA_ERROR_NOT_FOUND;
}

static fa_result mac_set_speed(void* h, uint32_t kbps)
{
    mac_drive* d = (mac_drive*)h;
    uint16_t v = (uint16_t)(kbps > 0xFFFF ? 0xFFFF : kbps);
    return ioctl(d->fd, DKIOCCDSETSPEED, &v) == 0 ? FA_OK : FA_ERROR_DEVICE;
}

static fa_result mac_get_speed(void* h, uint32_t* out_kbps)
{
    mac_drive* d = (mac_drive*)h;
    uint16_t v = 0;
    if (ioctl(d->fd, DKIOCCDGETSPEED, &v) != 0) return FA_ERROR_DEVICE;
    *out_kbps = v;
    return FA_OK;
}

static fa_result mac_tray(void* h, int open)
{
    mac_drive* d = (mac_drive*)h;
    if (!open) return FA_ERROR_NOT_IMPLEMENTED;
    /* Fails with EBUSY while Finder has the disc mounted. */
    if (ioctl(d->fd, DKIOCEJECT, NULL) == 0) return FA_OK;
    return errno == EBUSY ? FA_ERROR_BUSY : FA_ERROR_DEVICE;
}

static void mac_close(void* h)
{
    mac_drive* d = (mac_drive*)h;
    close(d->fd);
    free(d);
}

static const fa_disc_ops mac_ops = {
    mac_inquiry, mac_test_ready, mac_read_toc, mac_read_cd, mac_read_code,
    mac_set_speed, mac_get_speed, mac_tray, NULL, mac_close
};

uint32_t fa_disc_platform_list(char (*ids)[256], uint32_t cap)
{
    io_iterator_t it = 0;
    io_service_t svc;
    uint32_t n = 0;
    if (IOServiceGetMatchingServices(MACH_PORT_NULL, IOServiceMatching(kIOCDMediaClass), &it) != KERN_SUCCESS) return 0;
    while ((svc = IOIteratorNext(it)) != 0) {
        CFTypeRef name = IORegistryEntryCreateCFProperty(svc, CFSTR(kIOBSDNameKey), kCFAllocatorDefault, 0);
        if (name) {
            if (n < cap) cfstring_copy(name, ids[n], 256);
            n++;
            CFRelease(name);
        }
        IOObjectRelease(svc);
    }
    IOObjectRelease(it);
    return n;
}

fa_result fa_disc_platform_open(const char* id, const fa_disc_ops** out_ops, void** out_handle)
{
    char path[96];
    mac_drive* d;
    int fd;
    if (strncmp(id, "/dev/", 5) == 0) id += 5;
    if (id[0] == 'r' && strncmp(id, "rdisk", 5) == 0) id += 1;
    if (strncmp(id, "disk", 4) != 0 || strlen(id) > 32) return FA_ERROR_NOT_FOUND;
    snprintf(path, sizeof(path), "/dev/r%s", id);
    fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return errno == ENOENT ? FA_ERROR_NOT_FOUND : FA_ERROR_DEVICE;
    {
        /* Only CD media: the CD ioctls work on it. */
        uint16_t speed = 0;
        if (ioctl(fd, DKIOCCDGETSPEED, &speed) != 0) { close(fd); return FA_ERROR_NOT_FOUND; }
    }
    d = (mac_drive*)calloc(1, sizeof(*d));
    if (!d) { close(fd); return FA_ERROR_OUT_OF_MEMORY; }
    d->fd = fd;
    snprintf(d->bsd, sizeof(d->bsd), "%s", id);
    *out_ops = &mac_ops;
    *out_handle = d;
    return FA_OK;
}

#elif defined(__APPLE__)

uint32_t fa_disc_platform_list(char (*ids)[256], uint32_t cap) { (void)ids; (void)cap; return 0; }

fa_result fa_disc_platform_open(const char* id, const fa_disc_ops** out_ops, void** out_handle)
{
    (void)id; (void)out_ops; (void)out_handle;
    return FA_ERROR_NOT_FOUND;
}

#endif
