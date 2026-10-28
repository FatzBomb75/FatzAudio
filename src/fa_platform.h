/* Minimal portable threading layer: sequentially consistent atomics, mutexes,
 * condition variables with timeouts, threads, and a monotonic clock.
 * Everything the audio thread touches is in the atomics section: those never
 * block and never make system calls. */
#ifndef FA_PLATFORM_H
#define FA_PLATFORM_H

#include <stdint.h>
#include <stdio.h>

#if defined(_MSC_VER) && !defined(__clang__)
#  include <intrin.h>
#  define FA_MSVC_ATOMICS 1
#endif

/* ---- Atomics (seq_cst; lock-free on every supported target) ---------------- */

typedef struct { volatile int32_t v; } fa_atomic32;
#if defined(FA_MSVC_ATOMICS)
typedef struct { volatile int64_t v; } fa_atomic64;   /* MSVC aligns int64_t to 8 */
#else
/* 8-byte aligned even where int64_t is only 4 (32-bit x86 Linux and Android): a
 * misaligned 64-bit atomic can straddle two cache lines, a split lock. */
typedef struct { _Alignas(8) volatile int64_t v; } fa_atomic64;
#endif
typedef struct { void* volatile v; } fa_atomic_ptr;

#if defined(FA_MSVC_ATOMICS)
#if defined(_M_X64) || defined(_M_IX86)
/* x86: an aligned load is already acquire and seq_cst stores are locked (xchg),
 * so a seq_cst load is a plain load the compiler must not move (as MSVC's own
 * <atomic> does it), not a locked compare-exchange that takes the cache line. */
static __inline int32_t fa_load32(fa_atomic32* a)
{
    int32_t v;
    _ReadWriteBarrier();
    v = (int32_t)__iso_volatile_load32((const volatile __int32*)&a->v);
    _ReadWriteBarrier();
    return v;
}
#else
static __inline int32_t fa_load32(fa_atomic32* a) { return (int32_t)_InterlockedCompareExchange((volatile long*)&a->v, 0, 0); }
#endif
static __inline void fa_store32(fa_atomic32* a, int32_t x) { _InterlockedExchange((volatile long*)&a->v, (long)x); }
static __inline int32_t fa_fetch_add32(fa_atomic32* a, int32_t d) { return (int32_t)_InterlockedExchangeAdd((volatile long*)&a->v, (long)d); }
static __inline int32_t fa_exchange32(fa_atomic32* a, int32_t x) { return (int32_t)_InterlockedExchange((volatile long*)&a->v, (long)x); }
static __inline int fa_cas32(fa_atomic32* a, int32_t expected, int32_t desired)
{ return _InterlockedCompareExchange((volatile long*)&a->v, (long)desired, (long)expected) == (long)expected; }
#if defined(_M_X64)
static __inline int64_t fa_load64(fa_atomic64* a)
{
    int64_t v;
    _ReadWriteBarrier();
    v = (int64_t)__iso_volatile_load64((const volatile __int64*)&a->v);
    _ReadWriteBarrier();
    return v;
}
static __inline void fa_store64(fa_atomic64* a, int64_t x) { _InterlockedExchange64((volatile __int64*)&a->v, x); }
static __inline int64_t fa_fetch_add64(fa_atomic64* a, int64_t d) { return _InterlockedExchangeAdd64((volatile __int64*)&a->v, d); }
static __inline int64_t fa_exchange64(fa_atomic64* a, int64_t x) { return _InterlockedExchange64((volatile __int64*)&a->v, x); }
#else
/* 32-bit x86 (no plain 64-bit load or exchange) and ARM64: compare-exchange loops. */
static __inline int64_t fa_load64(fa_atomic64* a) { return _InterlockedCompareExchange64((volatile __int64*)&a->v, 0, 0); }
static __inline void fa_store64(fa_atomic64* a, int64_t x)
{
    __int64 old = a->v;
    for (;;) { __int64 seen = _InterlockedCompareExchange64((volatile __int64*)&a->v, x, old); if (seen == old) break; old = seen; }
}
static __inline int64_t fa_fetch_add64(fa_atomic64* a, int64_t d)
{
    __int64 old = a->v;
    for (;;) { __int64 seen = _InterlockedCompareExchange64((volatile __int64*)&a->v, old + d, old); if (seen == old) return old; old = seen; }
}
static __inline int64_t fa_exchange64(fa_atomic64* a, int64_t x)
{
    __int64 old = a->v;
    for (;;) { __int64 seen = _InterlockedCompareExchange64((volatile __int64*)&a->v, x, old); if (seen == old) return old; old = seen; }
}
#endif
static __inline int fa_cas64(fa_atomic64* a, int64_t expected, int64_t desired)
{ return _InterlockedCompareExchange64((volatile __int64*)&a->v, desired, expected) == expected; }
#if defined(_M_X64)
static __inline void* fa_load_ptr(fa_atomic_ptr* a) { return (void*)fa_load64((fa_atomic64*)(void*)a); }
#elif defined(_M_IX86)
static __inline void* fa_load_ptr(fa_atomic_ptr* a) { return (void*)(intptr_t)fa_load32((fa_atomic32*)(void*)a); }
#else
static __inline void* fa_load_ptr(fa_atomic_ptr* a) { return _InterlockedCompareExchangePointer((void* volatile*)&a->v, NULL, NULL); }
#endif
static __inline void* fa_exchange_ptr(fa_atomic_ptr* a, void* x) { return _InterlockedExchangePointer((void* volatile*)&a->v, x); }
static __inline int fa_cas_ptr(fa_atomic_ptr* a, void* expected, void* desired)
{ return _InterlockedCompareExchangePointer((void* volatile*)&a->v, desired, expected) == expected; }
#else
static inline int32_t fa_load32(fa_atomic32* a) { return __atomic_load_n(&a->v, __ATOMIC_SEQ_CST); }
static inline void fa_store32(fa_atomic32* a, int32_t x) { __atomic_store_n(&a->v, x, __ATOMIC_SEQ_CST); }
static inline int32_t fa_fetch_add32(fa_atomic32* a, int32_t d) { return __atomic_fetch_add(&a->v, d, __ATOMIC_SEQ_CST); }
static inline int32_t fa_exchange32(fa_atomic32* a, int32_t x) { return __atomic_exchange_n(&a->v, x, __ATOMIC_SEQ_CST); }
static inline int fa_cas32(fa_atomic32* a, int32_t expected, int32_t desired)
{ return __atomic_compare_exchange_n(&a->v, &expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
static inline int64_t fa_load64(fa_atomic64* a) { return __atomic_load_n(&a->v, __ATOMIC_SEQ_CST); }
static inline void fa_store64(fa_atomic64* a, int64_t x) { __atomic_store_n(&a->v, x, __ATOMIC_SEQ_CST); }
static inline int64_t fa_fetch_add64(fa_atomic64* a, int64_t d) { return __atomic_fetch_add(&a->v, d, __ATOMIC_SEQ_CST); }
static inline int64_t fa_exchange64(fa_atomic64* a, int64_t x) { return __atomic_exchange_n(&a->v, x, __ATOMIC_SEQ_CST); }
static inline int fa_cas64(fa_atomic64* a, int64_t expected, int64_t desired)
{ return __atomic_compare_exchange_n(&a->v, &expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
static inline void* fa_load_ptr(fa_atomic_ptr* a) { return __atomic_load_n(&a->v, __ATOMIC_SEQ_CST); }
static inline void* fa_exchange_ptr(fa_atomic_ptr* a, void* x) { return __atomic_exchange_n(&a->v, x, __ATOMIC_SEQ_CST); }
static inline int fa_cas_ptr(fa_atomic_ptr* a, void* expected, void* desired)
{ return __atomic_compare_exchange_n(&a->v, &expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
#endif

static inline float fa_load_float(fa_atomic32* a)
{
    union { int32_t i; float f; } u;
    u.i = fa_load32(a);
    return u.f;
}
static inline void fa_store_float(fa_atomic32* a, float f)
{
    union { int32_t i; float f; } u;
    u.f = f;
    fa_store32(a, u.i);
}

/* ---- Blocking primitives (never used on the audio thread) ------------------ */

typedef struct fa_mutex { void* impl; } fa_mutex;
typedef struct fa_cond { void* impl; } fa_cond;
typedef struct fa_thread { void* impl; } fa_thread;
typedef void (*fa_thread_proc)(void* arg);

int fa_mutex_init(fa_mutex* m);   /* 0 on success */
void fa_mutex_destroy(fa_mutex* m);
void fa_mutex_lock(fa_mutex* m);
int fa_mutex_trylock(fa_mutex* m); /* 1 if acquired */
void fa_mutex_unlock(fa_mutex* m);

int fa_cond_init(fa_cond* c);
void fa_cond_destroy(fa_cond* c);
/* Waits until signalled or `timeout_ms` passed; spurious wakeups are possible. */
void fa_cond_wait_ms(fa_cond* c, fa_mutex* m, uint32_t timeout_ms);   /* UINT32_MAX: no timeout */
void fa_cond_broadcast(fa_cond* c);
void fa_cond_signal(fa_cond* c);   /* wakes one waiter */

int fa_thread_create(fa_thread* t, fa_thread_proc proc, void* arg);
void fa_thread_join(fa_thread* t);
/* Lets the thread clean up after itself when it exits; `t` can no longer be joined. */
void fa_thread_detach(fa_thread* t);
/* Opaque id of the calling thread, comparable with fa_thread_id_of. */
uintptr_t fa_thread_current_id(void);

void fa_sleep_ms(uint32_t ms);

/* Files with UTF-8 paths (wide-character APIs on Windows). */
FILE* fa_fopen_utf8(const char* path, const char* mode);
/* An open file's identity and last-write time (device, file id, seconds, nanoseconds; Windows: volume, index,
 * FILETIME, 0). Two reads with the same identity saw the same bytes unless it changed within the clock's step.
 * 0 when the OS gives none. */
int fa_file_identity(FILE* fp, uint64_t id[4]);
int fa_mkdir_utf8(const char* path);                     /* 0 if created or already there */
int fa_replace_file(const char* from, const char* to);   /* rename over an existing file; 0 on success */
/* Where the OS keeps its MIDI instruments (gm.dls, gs_instruments.dls, a GM .sf2),
 * in the order to try: the `index`th path in UTF-8, 0 past the last one. */
int fa_system_soundfont_path(int index, char* out, size_t cap);

/* A new file in `folder` ("w+b") that the system deletes when it is closed, also
 * if the process dies; NULL on failure. */
FILE* fa_temp_file_create(const char* folder);
/* Deletes files fa_temp_file_create left behind (a power cut, a crash). */
void fa_temp_folder_sweep(const char* folder);
uint32_t fa_cpu_count(void);
int64_t fa_monotonic_ns(void);

/* Runs `init(ctx)` once per process; `state` is a zeroed static. init returns 1 when it is done, 0 when it failed
 * and a later call may retry, -1 when it failed for good. Callers that arrive meanwhile wait. Returns 1 once init
 * has succeeded, 0 otherwise. */
int fa_once(fa_atomic32* state, int (*init)(void* ctx), void* ctx);

/* A mutex for static data, created on first use (process-wide singletons). */
typedef struct fa_lazy_mutex { fa_atomic32 state; fa_mutex m; } fa_lazy_mutex;
void fa_lazy_mutex_lock(fa_lazy_mutex* l);
void fa_lazy_mutex_unlock(fa_lazy_mutex* l);

/* Shared libraries of the app loaded at runtime (LAME, libcurl); dynlib.c.
 * `already_loaded_only`: only a library already in the process (pinned there on
 * Windows, as LoadLibrary would). A path with a folder loads from that folder. */
void* fa_lib_open(const char* path, int already_loaded_only);
void* fa_lib_sym(void* lib, const char* name);
/* Gives back a library opened with fa_lib_open (same `already_loaded_only`) that
 * turned out unusable. */
void fa_lib_close(void* lib, int already_loaded_only);
/* Where `lib` was loaded from (found through `symbol`, one of its functions); ""
 * when the platform cannot tell. */
void fa_lib_where(void* lib, const char* symbol, char* out, size_t cap);

#endif
