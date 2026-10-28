/* Fuzz target: the tag readers (ID3v1/v2, APE, Vorbis comments, FLAC pictures, MP4,
 * RIFF INFO, AIFF, Matroska) and the loudness-tag reader, reading every item back. */
#include "fa_internal.h"
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    fa_tags* t = NULL;
    uint32_t flags, i, n;
    for (flags = 0; flags < 2; flags++) {
        if (fa_tags_read_memory(data, size, flags ? FA_TAGS_NO_INFO : 0, &t) != FA_OK) continue;
        n = fa_tags_get_item_count(t);
        for (i = 0; i < n; i++) {
            fa_tag_item it;
            if (fa_tags_get_item(t, i, &it) == FA_OK && it.key && it.value) (void)(strlen(it.key) + strlen(it.value));
        }
        n = fa_tags_get_picture_count(t);
        for (i = 0; i < n; i++) {
            fa_tag_picture p;
            if (fa_tags_get_picture(t, i, &p) == FA_OK && p.data && p.size) (void)*(volatile const uint8_t*)p.data, (void)((const volatile uint8_t*)p.data)[p.size - 1];
        }
        for (i = 0; i < 24; i++) (void)fa_tags_get(t, (fa_tag_field)i);
        {
            fa_stream_info info;
            fa_tags_get_info(t, &info);
        }
        fa_tags_close(t);
        t = NULL;
    }
    {
        fa_source src;
        fa_tag_loudness l;
        if (fa_source_open_memory_ref(&src, data, size) == FA_OK) {
            fa_tags_read_loudness(&src, &l);
            fa_source_close(&src);
        }
    }
    return 0;
}
