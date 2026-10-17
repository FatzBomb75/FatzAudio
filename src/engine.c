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
    ma_result r;
    if (!out_engine) return FA_ERROR_INVALID_ARG;
    *out_engine = NULL;

    e = (fa_engine*)calloc(1, sizeof(*e));
    if (!e) return FA_ERROR_OUT_OF_MEMORY;
    /* Copy only the part of the config the caller's struct_size covers, so callers
     * compiled against an older, shorter struct keep working. */
    fa_engine_config_init(&e->config);
    if (config) {
        size_t n = config->struct_size;
        if (n == 0 || n > sizeof(e->config)) n = sizeof(e->config);
        memcpy(&e->config, config, n);
        e->config.struct_size = (uint32_t)sizeof(e->config);
    }

    if (ma_mutex_init(&e->lock) != MA_SUCCESS) { free(e); return FA_ERROR_GENERIC; }
    if (e->config.flags & FA_ENGINE_NULL_OUTPUT) {
        ma_backend null_backend = ma_backend_null;
        r = ma_context_init(&null_backend, 1, NULL, &e->context);
    } else {
        r = ma_context_init(NULL, 0, NULL, &e->context);
    }
    if (r != MA_SUCCESS) {
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
