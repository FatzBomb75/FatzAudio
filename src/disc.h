/* Audio CD access, internal interfaces.
 *
 *   disc.c        the public fa_disc_* API: TOC, CD-Text, disc ids, error-tolerant
 *                 audio reads, and the track decoder behind fa_stream_open_disc
 *   disc_mmc.c    the backend operations as SCSI MMC commands, for platforms where
 *                 the OS passes commands through (Linux SG_IO, Windows SPTI)
 *   disc_linux.c  /dev/sr* through SG_IO
 *   disc_win.c    \\.\D: through IOCTL_SCSI_PASS_THROUGH
 *   disc_mac.c    /dev/rdiskN through the IOCDMedia ioctls (DKIOCCD*)
 *   disc_image.c  .cue/.bin disc images
 *
 * A backend implements fa_disc_ops; everything above it is shared. */
#ifndef FA_DISC_H
#define FA_DISC_H

#include "fa_internal.h"

#define FA_CD_SECTOR_BYTES 2352     /* audio bytes per sector */
#define FA_CD_C2_BYTES 294          /* one error bit per audio byte */
#define FA_CD_SUB_BYTES 96          /* raw P-W subcode */
#define FA_CD_PREGAP 150            /* sectors before LBA 0 (MSF 00:02:00) */
#define FA_CD_SESSION_GAP 11400     /* lead-out + lead-in + pregap between sessions */

/* Parts of a sector read_cd returns, in this order per sector: audio (2352 bytes),
 * C2 error pointers (294), raw P-W subcode (96). */
enum {
    FA_CD_AREA_AUDIO = 1 << 0,
    FA_CD_AREA_C2    = 1 << 1,
    FA_CD_AREA_SUB   = 1 << 2
};

/* fa_disc_ops.test_ready */
enum { FA_CD_READY = 0, FA_CD_NO_DISC, FA_CD_TRAY_OPEN, FA_CD_SPINNING_UP };

/* fa_disc_ops.read_code */
enum { FA_CD_CODE_MCN = 2, FA_CD_CODE_ISRC = 3 };

typedef struct fa_disc_ops {
    /* Identification, NUL-terminated and trimmed. */
    fa_result (*inquiry)(void* h, char vendor[16], char product[32], char revision[8]);
    /* *out_state = FA_CD_*; *out_changed = 1 when the drive reported a media change. */
    fa_result (*test_ready)(void* h, int* out_state, int* out_changed);
    /* The response of MMC READ TOC/PMA/ATIP (LBA addressing) for `format` (0 TOC,
     * 1 session info, 5 CD-Text), including its 4-byte header. */
    fa_result (*read_toc)(void* h, int format, int track, uint8_t* buf, uint32_t cap, uint32_t* out_len);
    /* `count` (up to 32) CD-DA sectors from `lba`, with the FA_CD_AREA_* parts in `areas`.
     * FA_ERROR_NOT_IMPLEMENTED when the drive cannot return a requested part
     * (the caller retries without it), FA_ERROR_IO for a read error,
     * FA_ERROR_NO_DISC when the disc is gone. */
    fa_result (*read_cd)(void* h, int64_t lba, uint32_t count, uint32_t areas, uint8_t* buf);
    /* Media catalog number (13 digits) or a track's ISRC (12 characters) from the
     * Q subcode. FA_ERROR_NOT_FOUND when the disc has none. */
    fa_result (*read_code)(void* h, int which, int track, char* out, size_t cap);
    fa_result (*set_speed)(void* h, uint32_t kbytes_per_s);   /* 0xFFFF = fastest */
    fa_result (*get_speed)(void* h, uint32_t* out_kbytes_per_s);
    fa_result (*tray)(void* h, int open);
    fa_result (*lock)(void* h, int locked);
    void (*close)(void* h);
} fa_disc_ops;

/* Opens a disc over a backend; takes ownership of `h` (closed on failure too). */
fa_result fa_disc_open_ops(const fa_disc_ops* ops, void* h, const fa_disc_options* options, fa_disc** out_disc);

/* ---- MMC (disc_mmc.c) --------------------------------------------------------- */

enum { FA_MMC_NONE = 0, FA_MMC_IN, FA_MMC_OUT };

typedef struct fa_mmc_sense {
    uint8_t key, asc, ascq;
} fa_mmc_sense;

/* Runs one command. FA_OK, FA_ERROR_IO with `sense` filled when the drive
 * answered CHECK CONDITION, FA_ERROR_DEVICE when the OS call itself failed. */
typedef fa_result (*fa_mmc_exec_fn)(void* h, const uint8_t* cdb, int cdb_len, void* buf, uint32_t len, int dir,
                                    fa_mmc_sense* sense, uint32_t timeout_ms);

/* Key, ASC and ASCQ from fixed- or descriptor-format sense data. */
void fa_mmc_parse_sense(const uint8_t* data, size_t len, fa_mmc_sense* out);

/* Backend operations as MMC commands over `exec`. `close` releases `h`.
 * `max_sectors` is the largest read the OS passes through in one command. */
fa_result fa_mmc_open(fa_mmc_exec_fn exec, void (*close)(void*), void* h, uint32_t max_sectors,
                      const fa_disc_ops** out_ops, void** out_handle);

/* ---- Platform drives (disc_linux.c, disc_win.c, disc_mac.c) ------------------- */

/* Fills up to `cap` drive ids; returns how many exist. */
uint32_t fa_disc_platform_list(char (*ids)[256], uint32_t cap);
fa_result fa_disc_platform_open(const char* id, const fa_disc_ops** out_ops, void** out_handle);

/* ---- Images (disc_image.c) ----------------------------------------------------- */

fa_result fa_disc_image_open(const char* cue_path, const fa_disc_ops** out_ops, void** out_handle);

/* ---- Track streams (disc.c) ------------------------------------------------------ */

/* Opens the decoder of an audio track for fa_stream_open_disc. The decoder holds a
 * reference to the disc until it is closed. `stream` receives read-error events. */
fa_result fa_disc_decoder_open(fa_decoder* dec, fa_source* src, fa_disc* disc, int32_t track, struct fa_stream* stream);
/* A Windows TrackNN.cda file (44 bytes, RIFF "CDDA"): its track number, or 0. */
int fa_disc_cda_track(const uint8_t* data, size_t size);

/* ---- Shared helpers (disc.c) ----------------------------------------------------- */

/* CD-Text packs (18 bytes each) to per-track strings. Exposed for disc_image.c,
 * which builds packs for the CUE sheet's TITLE/PERFORMER/... lines. */
uint16_t fa_cdtext_crc(const uint8_t* pack16);
/* Trims trailing spaces and NULs of a fixed-width field into `out`. */
void fa_disc_copy_trimmed(char* out, size_t cap, const uint8_t* in, size_t n);

#endif
