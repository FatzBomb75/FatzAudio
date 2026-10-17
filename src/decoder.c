/* Decoder registry: one table of built-in formats and one auto-detecting open. */
#include "fa_internal.h"
#include <stdio.h>
#include <string.h>

const fa_decoder_vtable* const fa_decoder_registry[] = {
    &fa_decoder_wav,
    &fa_decoder_flac,
    &fa_decoder_mp3,
};
const uint32_t fa_decoder_registry_count = (uint32_t)(sizeof(fa_decoder_registry) / sizeof(fa_decoder_registry[0]));

/* Is `item` one of the ';'-separated entries of `list` (ASCII case-insensitive)? */
static int list_contains(const char* list, const char* item, size_t item_len)
{
    while (list && *list) {
        const char* end = strchr(list, ';');
        size_t len = end ? (size_t)(end - list) : strlen(list);
        if (len == item_len) {
            size_t i;
            for (i = 0; i < len; i++) {
                char a = list[i], b = item[i];
                if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
                if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
                if (a != b) break;
            }
            if (i == len) return 1;
        }
        list = end ? end + 1 : NULL;
    }
    return 0;
}

static const char* extension_of(const char* path)
{
    const char* dot;
    const char* slash;
    if (!path) return NULL;
    dot = strrchr(path, '.');
    slash = strrchr(path, '/');
    if (!slash) slash = strrchr(path, '\\');
    if (!dot || (slash && dot < slash)) return NULL;
    return dot + 1;
}

fa_result fa_decoder_open(fa_decoder* dec, fa_source* src, const char* name_hint, const char* mime_hint)
{
    const uint8_t* head;
    size_t n, id3;
    int scores[16];
    int tried[16];
    uint32_t i, count = fa_decoder_registry_count;
    const char* ext = extension_of(name_hint);
    fa_result last = FA_ERROR_UNSUPPORTED_FORMAT;

    if (count > 16) count = 16;
    memset(dec, 0, sizeof(*dec));
    n = fa_source_peek(src, &head);
    if (n == 0) return src->length == 0 ? FA_ERROR_UNSUPPORTED_FORMAT : FA_ERROR_IO;
    id3 = fa_probe_id3v2_size(head, n);

    for (i = 0; i < count; i++) {
        const fa_decoder_vtable* vt = fa_decoder_registry[i];
        int s = vt->probe(head, n, id3);
        /* Name and mime type only break ties between formats the content allows. */
        if (s > 0 && ext && list_contains(vt->extensions, ext, strlen(ext))) s += 2;
        if (s > 0 && mime_hint && list_contains(vt->mime_types, mime_hint, strlen(mime_hint))) s += 1;
        scores[i] = s;
        tried[i] = 0;
    }

    /* Best score first; if that decoder rejects the data, the next candidate gets a go. */
    for (;;) {
        int best = -1;
        fa_result r;
        for (i = 0; i < count; i++)
            if (!tried[i] && scores[i] > 0 && (best < 0 || scores[i] > scores[best])) best = (int)i;
        if (best < 0) break;
        tried[best] = 1;

        memset(dec, 0, sizeof(*dec));
        dec->vt = fa_decoder_registry[best];
        dec->src = src;
        dec->length = -1;
        if (fa_source_seek(src, 0) != FA_OK) return FA_ERROR_NOT_SEEKABLE;
        r = dec->vt->open(dec);
        if (r == FA_OK) {
            dec->format.sample_format = FA_FORMAT_F32;
            if (!dec->codec[0]) snprintf(dec->codec, sizeof(dec->codec), "%s", dec->vt->name);
            if (!dec->container[0]) snprintf(dec->container, sizeof(dec->container), "%s", dec->vt->name);
            return FA_OK;
        }
        if (r == FA_ERROR_OUT_OF_MEMORY || r == FA_ERROR_IO) return r;
        last = r;
    }
    memset(dec, 0, sizeof(*dec));
    return last == FA_OK ? FA_ERROR_UNSUPPORTED_FORMAT : last;
}

fa_result fa_decoder_read(fa_decoder* dec, float* out, fa_frames frames, fa_frames* got)
{
    *got = 0;
    if (frames <= 0) return FA_OK;
    return dec->vt->read(dec, out, frames, got);
}

fa_result fa_decoder_seek(fa_decoder* dec, fa_frames frame)
{
    if (!dec->seekable) return FA_ERROR_NOT_SEEKABLE;
    if (frame < 0 || (dec->length >= 0 && frame > dec->length)) return FA_ERROR_INVALID_ARG;
    return dec->vt->seek(dec, frame);
}

void fa_decoder_close(fa_decoder* dec)
{
    if (dec->vt) dec->vt->close(dec);
    memset(dec, 0, sizeof(*dec));
}

/* ---- Public format list ---------------------------------------------------- */

FA_API uint32_t FA_CALL fa_format_count(void)
{
    return fa_decoder_registry_count;
}

FA_API fa_result FA_CALL fa_format_get_info(uint32_t index, fa_format_info* out_info)
{
    const fa_decoder_vtable* vt;
    if (!out_info) return FA_ERROR_INVALID_ARG;
    memset(out_info, 0, sizeof(*out_info));
    if (index >= fa_decoder_registry_count) return FA_ERROR_NOT_FOUND;
    vt = fa_decoder_registry[index];
    snprintf(out_info->name, sizeof(out_info->name), "%s", vt->name);
    snprintf(out_info->description, sizeof(out_info->description), "%s", vt->description);
    snprintf(out_info->extensions, sizeof(out_info->extensions), "%s", vt->extensions);
    snprintf(out_info->mime_types, sizeof(out_info->mime_types), "%s", vt->mime_types);
    return FA_OK;
}
