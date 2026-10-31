/* Fuzz target: the ZIP reader. Lists the central directory (ZIP64 included) and
 * unpacks every entry it lists, in memory and to a temp file, and probes its first bytes. */
#include "cdg.h"
#include <stdio.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    fa_source src;
    fa_zip_entry* e = NULL;
    uint32_t n = 0, i;
    (void)fa_zip_probe(data, size);
    if (fa_source_open_memory_ref(&src, data, size) != FA_OK) return 0;
    if (fa_zip_list(&src, &e, &n) == FA_OK) {
        for (i = 0; i < n && i < 8; i++) {
            uint8_t* out = NULL;
            int64_t off;
            uint8_t head[512];
            size_t got = 0;
            (void)fa_zip_entry_offset(&src, &e[i], &off);
            (void)fa_zip_extract_prefix(&src, &e[i], head, sizeof(head), &got);
            (void)fa_decoder_recognizes(head, got);
            if (fa_zip_extract(&src, &e[i], (uint64_t)4 << 20, NULL, &out) == FA_OK) free(out);
            if (e[i].size <= ((uint64_t)1 << 22)) {
                FILE* f = tmpfile();
                if (f) { (void)fa_zip_extract_file(&src, &e[i], NULL, f); fclose(f); }
            }
        }
        free(e);
    }
    fa_source_close(&src);
    return 0;
}
