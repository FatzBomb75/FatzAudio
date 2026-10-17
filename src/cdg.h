/* CD+G graphics (cdg.c) and the ZIP reader behind karaoke archives (zip.c). */
#ifndef FA_CDG_H
#define FA_CDG_H

#include "fa_internal.h"

/* ---- ZIP archives (zip.c) ------------------------------------------------- */

typedef struct fa_zip_entry {
    char name[260];          /* path inside the archive, '/'-separated, UTF-8 or CP437 */
    uint32_t method;         /* 0 stored, 8 deflate; anything else cannot be read */
    uint32_t crc32;
    uint64_t compressed_size;
    uint64_t size;
    uint64_t header_offset;  /* of the local file header */
} fa_zip_entry;

/* 1 when `data` starts like a ZIP archive. */
int fa_zip_probe(const uint8_t* data, size_t size);
/* Reads the central directory. *out_entries is malloc'ed (free it), directories are
 * skipped. FA_ERROR_UNSUPPORTED_FORMAT for damaged, encrypted or ZIP64 archives. */
fa_result fa_zip_list(const uint8_t* zip, size_t zip_size, fa_zip_entry** out_entries, uint32_t* out_count);
/* Extracts one entry into a malloc'ed buffer, CRC checked. */
fa_result fa_zip_extract(const uint8_t* zip, size_t zip_size, const fa_zip_entry* entry, uint8_t** out_data);
/* Raw DEFLATE (RFC 1951). FA_OK only when the stream ends exactly at out_size bytes. */
fa_result fa_inflate(const uint8_t* in, size_t in_size, uint8_t* out, size_t out_size);
uint32_t fa_crc32(const uint8_t* data, size_t size);

/* ---- CD+G (cdg.c) ----------------------------------------------------------- */

#define FA_CDG_PACK_BYTES 24
#define FA_CDG_PACKS_PER_SECOND 300

/* The graphics of a stream: the raw packs, read in full (a song's .cdg is a few MB). */
typedef struct fa_cdg_data {
    uint8_t* packs;
    size_t count;            /* whole packs */
} fa_cdg_data;

/* Takes ownership of `bytes` (malloc'ed) on success and on failure. */
fa_result fa_cdg_data_adopt(uint8_t* bytes, size_t size, fa_cdg_data** out);
fa_result fa_cdg_data_from_path(const char* path, fa_cdg_data** out);
void fa_cdg_data_free(fa_cdg_data* data);
/* 1 when the file name ends in ".cdg" (any case). */
int fa_cdg_is_cdg_name(const char* name);

/* Stream glue. fa_stream_cdg_attach hands `data` to the stream (replacing what it
 * had; NULL removes the graphics). fa_stream_cdg_close, at fa_stream_close, frees
 * the data and detaches the surfaces (the app still destroys them). */
void fa_stream_cdg_attach(struct fa_stream* s, fa_cdg_data* data);
void fa_stream_cdg_close(struct fa_stream* s);
int fa_stream_cdg_present(struct fa_stream* s);
/* Looks for "<path without extension>.cdg" next to an audio file. */
fa_cdg_data* fa_cdg_find_sibling(const char* audio_path);

#endif
