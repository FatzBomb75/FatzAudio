/* Fuzz target: the HLS playlist parser (master and media, byte ranges, keys, init
 * sections) and the MPEG-TS audio extractor. The input is split at its first NUL:
 * before it a playlist, after it TS bytes (a plain playlist if there is no NUL). */
#include "fa_internal.h"
#include "net.h"
#include <stdlib.h>
#include <string.h>

static int sink(void* user, const uint8_t* p, size_t n)
{
    (void)p;
    *(size_t*)user += n;
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    const uint8_t* nul = (const uint8_t*)memchr(data, 0, size);
    size_t plen = nul ? (size_t)(nul - data) : size;
    fa_hls_playlist p;
    memset(&p, 0, sizeof(p));
    if (fa_hls_parse((const char*)data, plen, "http://h.com/m/master.m3u8", &p) == FA_OK) {
        (void)fa_hls_pick_variant(&p, 0);
        (void)fa_hls_pick_variant(&p, 128000);
        fa_hls_playlist_free(&p);
    }
    if (nul) {
        fa_ts_demux d;
        size_t total = 0, off = 0;
        uint8_t key[16] = { 1 }, iv[16] = { 2 };
        fa_ts_init(&d);
        /* Feed in uneven pieces so packets split across calls. */
        while (off < size - plen - 1) {
            size_t n = 1 + (off * 7 + 13) % 400;
            if (n > size - plen - 1 - off) n = size - plen - 1 - off;
            if (fa_ts_feed(&d, nul + 1 + off, n, sink, &total)) break;
            off += n;
        }
        if (size - plen - 1 >= 16) {
            uint8_t* c = (uint8_t*)malloc(size - plen - 1);
            size_t n = (size - plen - 1) & ~(size_t)15;
            memcpy(c, nul + 1, size - plen - 1);
            fa_aes128_cbc_decrypt(key, iv, c, n);
            free(c);
        }
    }
    return 0;
}
