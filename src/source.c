/* Byte sources behind every stream: a file, a memory block or app callbacks.
 * The first FA_PROBE_BYTES are kept in a prefix buffer so format detection can
 * look at them and rewind even when the app's callbacks cannot seek. */
#if !defined(_WIN32)
#  define _FILE_OFFSET_BITS 64       /* 64-bit off_t on 32-bit Linux/Android */
#  define _POSIX_C_SOURCE 200809L    /* fseeko/ftello under strict C11 */
#endif
#include "fa_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

static int file_seek(FILE* f, int64_t offset, int origin)
{
#ifdef _WIN32
    return _fseeki64(f, offset, origin);
#else
    return fseeko(f, (off_t)offset, origin);
#endif
}

static int64_t file_tell(FILE* f)
{
#ifdef _WIN32
    return _ftelli64(f);
#else
    return (int64_t)ftello(f);
#endif
}

fa_result fa_source_open_path(fa_source* src, const char* path)
{
    memset(src, 0, sizeof(*src));
    src->kind = FA_SOURCE_FILE;
    src->file = fa_fopen_utf8(path, "rb");
    if (!src->file) return FA_ERROR_NOT_FOUND;
    src->seekable = 1;
    src->length = -1;
    if (file_seek(src->file, 0, SEEK_END) == 0) {
        src->length = file_tell(src->file);
        file_seek(src->file, 0, SEEK_SET);
    }
    return FA_OK;
}

fa_result fa_source_open_memory(fa_source* src, const void* data, size_t size)
{
    memset(src, 0, sizeof(*src));
    src->kind = FA_SOURCE_MEMORY;
    src->owned = (uint8_t*)malloc(size);
    if (!src->owned) return FA_ERROR_OUT_OF_MEMORY;
    memcpy(src->owned, data, size);
    src->mem = src->owned;
    src->mem_size = size;
    src->seekable = 1;
    src->length = (int64_t)size;
    return FA_OK;
}

fa_result fa_source_open_memory_ref(fa_source* src, const void* data, size_t size)
{
    memset(src, 0, sizeof(*src));
    src->kind = FA_SOURCE_MEMORY;
    src->mem = (const uint8_t*)data;
    src->mem_size = size;
    src->seekable = 1;
    src->length = (int64_t)size;
    return FA_OK;
}

fa_result fa_source_open_io(fa_source* src, const fa_io_callbacks* io, void* user)
{
    memset(src, 0, sizeof(*src));
    src->kind = FA_SOURCE_IO;
    src->io = *io;
    src->io_user = user;
    src->seekable = io->seek != NULL;
    src->length = io->length ? io->length(user) : -1;
    if (src->length < 0) src->length = -1;
    return FA_OK;
}

/* Reads from the underlying callbacks at the raw cursor. */
static int64_t io_read_raw(fa_source* src, void* buf, size_t n)
{
    size_t done = 0;
    int64_t err = 0;
    while (done < n) {
        int64_t r = src->io.read(src->io_user, (uint8_t*)buf + done, n - done);
        if (r < 0) { err = r; break; }
        if (r == 0) break;
        done += (size_t)r;
    }
    /* Count what was read even when a later call failed: the app's cursor moved. */
    src->raw_pos += (int64_t)done;
    if (err < 0) {
        if (src->error == FA_OK) src->error = (fa_result)err;
        if (done == 0) return err;
    }
    return (int64_t)done;
}

size_t fa_source_peek(fa_source* src, const uint8_t** out_data)
{
    if (!src->prefix_filled) {
        int64_t got = 0;
        src->prefix_filled = 1;
        if (src->kind == FA_SOURCE_MEMORY) {
            got = (int64_t)(src->mem_size < FA_PROBE_BYTES ? src->mem_size : FA_PROBE_BYTES);
            memcpy(src->prefix, src->mem, (size_t)got);
        } else if (src->kind == FA_SOURCE_FILE) {
            file_seek(src->file, 0, SEEK_SET);
            got = (int64_t)fread(src->prefix, 1, FA_PROBE_BYTES, src->file);
            file_seek(src->file, src->pos, SEEK_SET);
        } else if (src->pos == 0 && src->raw_pos == 0) {
            got = io_read_raw(src, src->prefix, FA_PROBE_BYTES);
            if (got < 0) got = 0;
        }
        src->prefix_len = (size_t)got;
    }
    *out_data = src->prefix;
    return src->prefix_len;
}

int64_t fa_source_read(fa_source* src, void* buf, size_t n)
{
    uint8_t* out = (uint8_t*)buf;
    size_t done = 0;
    if (n == 0) return 0;

    if (src->kind == FA_SOURCE_MEMORY) {
        size_t avail = src->pos >= (int64_t)src->mem_size ? 0 : src->mem_size - (size_t)src->pos;
        if (n > avail) n = avail;
        memcpy(out, src->mem + src->pos, n);
        src->pos += (int64_t)n;
        return (int64_t)n;
    }
    if (src->kind == FA_SOURCE_FILE) {
        size_t got = fread(out, 1, n, src->file);
        src->pos += (int64_t)got;
        if (ferror(src->file)) {
            if (src->error == FA_OK) src->error = FA_ERROR_IO;
            if (got == 0) return FA_ERROR_IO;
        }
        return (int64_t)got;
    }

    /* Callbacks: serve the buffered prefix first. */
    if (src->pos < (int64_t)src->prefix_len) {
        size_t k = src->prefix_len - (size_t)src->pos;
        if (k > n) k = n;
        memcpy(out, src->prefix + src->pos, k);
        src->pos += (int64_t)k;
        done = k;
        if (done == n) return (int64_t)done;
    }
    if (src->raw_pos != src->pos) {
        if (!src->seekable) return done > 0 ? (int64_t)done : FA_ERROR_NOT_SEEKABLE;
        if (src->io.seek(src->io_user, src->pos) != FA_OK) {
            if (src->error == FA_OK) src->error = FA_ERROR_IO;
            return done > 0 ? (int64_t)done : FA_ERROR_IO;
        }
        src->raw_pos = src->pos;
    }
    {
        int64_t r = io_read_raw(src, out + done, n - done);
        if (r < 0) return done > 0 ? (int64_t)done : r;
        src->pos += r;
        done += (size_t)r;
    }
    return (int64_t)done;
}

fa_result fa_source_seek(fa_source* src, int64_t offset)
{
    if (offset < 0) return FA_ERROR_INVALID_ARG;
    switch (src->kind) {
    case FA_SOURCE_MEMORY:
        if (offset > (int64_t)src->mem_size) return FA_ERROR_INVALID_ARG;
        break;
    case FA_SOURCE_FILE:
        if (file_seek(src->file, offset, SEEK_SET) != 0) return FA_ERROR_IO;
        break;
    default:
        if (src->seekable && offset >= (int64_t)src->prefix_len) {
            /* Ask the app now, so a seek it rejects fails here rather than on a later read. */
            if (src->io.seek(src->io_user, offset) != FA_OK) return FA_ERROR_IO;
            src->raw_pos = offset;
            break;
        }
        /* Inside the prefix, or forward on a non-seekable source, is always possible;
         * the actual repositioning happens lazily on the next read. */
        if (!src->seekable && offset > (int64_t)src->prefix_len && offset != src->raw_pos) {
            if (offset < src->raw_pos) return FA_ERROR_NOT_SEEKABLE;
            /* Skip forward by reading. */
            if (src->pos < src->raw_pos) src->pos = src->raw_pos;
            while (src->raw_pos < offset) {
                uint8_t tmp[4096];
                int64_t want = offset - src->raw_pos, r;
                if (want > (int64_t)sizeof(tmp)) want = (int64_t)sizeof(tmp);
                r = io_read_raw(src, tmp, (size_t)want);
                if (r <= 0) return FA_ERROR_IO;
            }
        }
        if (!src->seekable && offset < (int64_t)src->prefix_len && src->raw_pos > (int64_t)src->prefix_len)
            return FA_ERROR_NOT_SEEKABLE; /* prefix replay would not join up with the raw cursor */
        break;
    }
    src->pos = offset;
    return FA_OK;
}

int64_t fa_source_tell(const fa_source* src) { return src->pos; }

fa_result fa_source_read_all(fa_source* src, size_t max_size, uint8_t** out_data, size_t* out_size)
{
    int64_t len = src->length;
    uint8_t* buf;
    size_t done = 0;
    *out_data = NULL;
    *out_size = 0;
    if (len <= 0 || (uint64_t)len > (uint64_t)max_size) return FA_ERROR_UNSUPPORTED_FORMAT;
    buf = (uint8_t*)malloc((size_t)len);
    if (!buf) return FA_ERROR_OUT_OF_MEMORY;
    if (fa_source_seek(src, 0) != FA_OK) { free(buf); return FA_ERROR_IO; }
    while (done < (size_t)len) {
        int64_t got = fa_source_read(src, buf + done, (size_t)len - done);
        if (got <= 0) { free(buf); return FA_ERROR_IO; }
        done += (size_t)got;
    }
    *out_data = buf;
    *out_size = done;
    return FA_OK;
}

void fa_source_close(fa_source* src)
{
    if (src->kind == FA_SOURCE_FILE && src->file) fclose(src->file);
    if (src->kind == FA_SOURCE_IO && src->io.close) src->io.close(src->io_user);
    free(src->owned);
    memset(src, 0, sizeof(*src));
}
