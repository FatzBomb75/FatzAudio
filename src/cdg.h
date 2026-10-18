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

/* Compressed entries are unpacked into memory up to this size unless the engine
 * says otherwise (fa_engine_config.zip_memory_limit_mb). */
#define FA_ZIP_DEFAULT_MEMORY_LIMIT ((uint64_t)256 << 20)

/* 1 when `data` starts like a ZIP archive. */
int fa_zip_probe(const uint8_t* data, size_t size);
/* Reads the central directory of the archive `src` (seekable, known length): the
 * end of the file and the directory only. *out_entries is malloc'ed (free it),
 * directories are skipped. FA_ERROR_UNSUPPORTED_FORMAT for damaged or multi-part
 * archives (ZIP64 is read), FA_ERROR_NOT_SEEKABLE for a source that cannot seek. */
fa_result fa_zip_list(fa_source* src, fa_zip_entry** out_entries, uint32_t* out_count);
/* Where entry `e`'s data starts in `src`, after checking its local header. */
fa_result fa_zip_entry_offset(fa_source* src, const fa_zip_entry* e, int64_t* out_offset);
/* Unpacks one entry into a malloc'ed buffer, CRC checked. FA_ERROR_TOO_LARGE when
 * it is bigger than max_size. */
fa_result fa_zip_extract(fa_source* src, const fa_zip_entry* e, uint64_t max_size, uint8_t** out_data);
/* Unpacks one entry into `f` (written from its current position), CRC checked;
 * memory use stays at about 1 MB whatever the size. */
fa_result fa_zip_extract_file(fa_source* src, const fa_zip_entry* e, FILE* f);
/* Raw DEFLATE (RFC 1951). FA_OK only when the stream ends exactly at out_size bytes. */
fa_result fa_inflate(const uint8_t* in, size_t in_size, uint8_t* out, size_t out_size);
uint32_t fa_crc32(const uint8_t* data, size_t size);

/* ---- CD+G (cdg.c) ----------------------------------------------------------- */

#define FA_CDG_PACK_BYTES 24
#define FA_CDG_PACKS_PER_SECOND 300

/* The graphics of a stream: the raw packs, read in full (a song's .cdg is a few MB),
 * and the checkpoints every surface of the stream seeks from (cdg.c). */
typedef struct fa_cdg_data {
    uint8_t* packs;
    size_t count;            /* whole packs */
    struct fa_cdg_index* index; /* NULL until built (or out of memory: seeks replay from the start) */
    int from_disc;           /* read from a disc's subcode while it plays */
    int disc_parity;         /* from_disc: the subcode layout was chosen by the packs' parity */
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
/* Graphics that arrive while the stream plays (audio CDs read the packs with the
 * audio): fa_cdg_data_blank makes `count` zero packs (no-ops) whose checkpoints are
 * taken as the writes arrive;
 * fa_stream_cdg_attach_if_none attaches them unless the stream already has graphics
 * (returns 0 and frees `data` then), and gives a token for fa_stream_cdg_write,
 * which fills packs [first, first + n). Surfaces already past `first` go back to
 * replay them. The write returns 0, writing nothing, once the stream's graphics were
 * replaced (the app loaded others). */
fa_result fa_cdg_data_blank(size_t count, fa_cdg_data** out);
int fa_stream_cdg_attach_if_none(struct fa_stream* s, fa_cdg_data* data, uint32_t* out_token);
int fa_stream_cdg_write(struct fa_stream* s, uint32_t token, size_t first, const uint8_t* packs, size_t n);
void fa_stream_cdg_close(struct fa_stream* s);
/* The event thread, every round (never with list_lock held: the clock of a push
 * stream takes api_lock): raises FA_EVENT_CDG_CHANGED for a stream whose shown
 * packs changed since the last call. */
void fa_stream_cdg_watch(struct fa_stream* s);
int fa_stream_cdg_present(struct fa_stream* s);
/* Looks for "<path without extension>.cdg" next to an audio file. */
fa_cdg_data* fa_cdg_find_sibling(const char* audio_path);

#endif
