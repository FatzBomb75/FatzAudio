/* Shared libraries of the app, loaded at runtime (FFmpeg, LAME): never linked. */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE   /* dladdr */
#endif
#include "fa_platform.h"
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

#ifdef _WIN32
void* fa_lib_open(const char* path, int already_loaded_only)
{
    wchar_t w[600];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 600)) return NULL;
    if (already_loaded_only) {
        HMODULE h = GetModuleHandleW(w);
        if (h) {
            HMODULE pinned = NULL;  /* keep it loaded for good, like LoadLibrary would */
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, w, &pinned);
        }
        return (void*)h;
    }
    return (void*)LoadLibraryExW(w, NULL, (strchr(path, '\\') || strchr(path, '/')) ? LOAD_WITH_ALTERED_SEARCH_PATH : 0);
}

void* fa_lib_sym(void* lib, const char* name) { return (void*)GetProcAddress((HMODULE)lib, name); }

void fa_lib_where(void* lib, const char* symbol, char* out, size_t cap)
{
    wchar_t w[512];
    DWORD n = GetModuleFileNameW((HMODULE)lib, w, 512);
    (void)symbol;
    if (n == 0 || n >= 512 || !WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL)) out[0] = '\0';
}
#else
void* fa_lib_open(const char* path, int already_loaded_only)
{
#  ifdef RTLD_NOLOAD
    if (already_loaded_only) return dlopen(path, RTLD_NOW | RTLD_NOLOAD);
#  else
    if (already_loaded_only) return NULL;
#  endif
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}

void* fa_lib_sym(void* lib, const char* name) { return dlsym(lib, name); }

void fa_lib_where(void* lib, const char* symbol, char* out, size_t cap)
{
#  if defined(__GLIBC__) || defined(__APPLE__)
    Dl_info info;
    void* f = dlsym(lib, symbol);
    if (f && dladdr(f, &info) && info.dli_fname) { snprintf(out, cap, "%s", info.dli_fname); return; }
#  endif
    (void)lib;
    (void)symbol;
    out[0] = '\0';
}
#endif
