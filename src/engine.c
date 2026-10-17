#include "fa_internal.h"
#include <stdlib.h>
#include <string.h>

FA_API uint32_t FA_CALL fa_version(void)
{
    return ((uint32_t)FA_VERSION_MAJOR << 16) | ((uint32_t)FA_VERSION_MINOR << 8) | (uint32_t)FA_VERSION_PATCH;
}

FA_API const char* FA_CALL fa_result_string(fa_result result)
{
    switch (result) {
    case FA_OK: return "ok";
    case FA_ERROR_GENERIC: return "generic error";
    case FA_ERROR_INVALID_ARG: return "invalid argument";
    case FA_ERROR_OUT_OF_MEMORY: return "out of memory";
    case FA_ERROR_NOT_FOUND: return "not found";
    case FA_ERROR_UNSUPPORTED_FORMAT: return "unsupported format";
    case FA_ERROR_DEVICE: return "device error";
    case FA_ERROR_IO: return "i/o error";
    case FA_ERROR_NETWORK: return "network error";
    case FA_ERROR_NOT_SEEKABLE: return "not seekable";
    case FA_ERROR_BUSY: return "busy";
    case FA_ERROR_END_OF_STREAM: return "end of stream";
    case FA_ERROR_NOT_IMPLEMENTED: return "not implemented";
    case FA_ERROR_PROVIDER_MISSING: return "provider missing";
    default: return "unknown result";
    }
}

FA_API void FA_CALL fa_engine_config_init(fa_engine_config* config)
{
    if (!config) return;
    memset(config, 0, sizeof(*config));
    config->struct_size = (uint32_t)sizeof(*config);
}

FA_API fa_result FA_CALL fa_engine_create(const fa_engine_config* config, fa_engine** out_engine)
{
    fa_engine* e;
    if (!out_engine) return FA_ERROR_INVALID_ARG;
    *out_engine = NULL;

    e = (fa_engine*)calloc(1, sizeof(*e));
    if (!e) return FA_ERROR_OUT_OF_MEMORY;
    if (config) e->config = *config; else fa_engine_config_init(&e->config);

    if (ma_mutex_init(&e->lock) != MA_SUCCESS) { free(e); return FA_ERROR_GENERIC; }
    if (ma_context_init(NULL, 0, NULL, &e->context) != MA_SUCCESS) {
        ma_mutex_uninit(&e->lock);
        free(e);
        return FA_ERROR_DEVICE;
    }
    *out_engine = e;
    return FA_OK;
}

FA_API void FA_CALL fa_engine_destroy(fa_engine* engine)
{
    if (!engine) return;
    ma_context_uninit(&engine->context);
    ma_mutex_uninit(&engine->lock);
    free(engine);
}
