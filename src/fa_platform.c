#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#  define _DEFAULT_SOURCE           /* _SC_NPROCESSORS_ONLN on glibc */
#  if defined(__APPLE__)
#    define _DARWIN_C_SOURCE        /* _SC_NPROCESSORS_ONLN on Apple */
#  endif
#endif
#include "fa_platform.h"
#include <stdlib.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>

int fa_mutex_init(fa_mutex* m)
{
    SRWLOCK* l = (SRWLOCK*)malloc(sizeof(SRWLOCK));
    if (!l) return -1;
    InitializeSRWLock(l);
    m->impl = l;
    return 0;
}
void fa_mutex_destroy(fa_mutex* m) { free(m->impl); m->impl = NULL; }
void fa_mutex_lock(fa_mutex* m) { AcquireSRWLockExclusive((SRWLOCK*)m->impl); }
int fa_mutex_trylock(fa_mutex* m) { return TryAcquireSRWLockExclusive((SRWLOCK*)m->impl) ? 1 : 0; }
void fa_mutex_unlock(fa_mutex* m) { ReleaseSRWLockExclusive((SRWLOCK*)m->impl); }

int fa_cond_init(fa_cond* c)
{
    CONDITION_VARIABLE* cv = (CONDITION_VARIABLE*)malloc(sizeof(CONDITION_VARIABLE));
    if (!cv) return -1;
    InitializeConditionVariable(cv);
    c->impl = cv;
    return 0;
}
void fa_cond_destroy(fa_cond* c) { free(c->impl); c->impl = NULL; }
void fa_cond_wait_ms(fa_cond* c, fa_mutex* m, uint32_t timeout_ms)
{
    SleepConditionVariableSRW((CONDITION_VARIABLE*)c->impl, (SRWLOCK*)m->impl, timeout_ms, 0);
}
void fa_cond_broadcast(fa_cond* c) { WakeAllConditionVariable((CONDITION_VARIABLE*)c->impl); }

typedef struct { fa_thread_proc proc; void* arg; } thread_start;

static DWORD WINAPI thread_main(LPVOID p)
{
    thread_start s = *(thread_start*)p;
    free(p);
    s.proc(s.arg);
    return 0;
}

int fa_thread_create(fa_thread* t, fa_thread_proc proc, void* arg)
{
    thread_start* s = (thread_start*)malloc(sizeof(*s));
    HANDLE h;
    if (!s) return -1;
    s->proc = proc;
    s->arg = arg;
    h = CreateThread(NULL, 0, thread_main, s, 0, NULL);
    if (!h) { free(s); return -1; }
    t->impl = h;
    return 0;
}
void fa_thread_join(fa_thread* t)
{
    if (!t->impl) return;
    WaitForSingleObject((HANDLE)t->impl, INFINITE);
    CloseHandle((HANDLE)t->impl);
    t->impl = NULL;
}
void fa_thread_detach(fa_thread* t)
{
    if (!t->impl) return;
    CloseHandle((HANDLE)t->impl);
    t->impl = NULL;
}
uintptr_t fa_thread_current_id(void) { return (uintptr_t)GetCurrentThreadId(); }

void fa_sleep_ms(uint32_t ms) { Sleep(ms); }
uint32_t fa_cpu_count(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors > 0 ? (uint32_t)si.dwNumberOfProcessors : 1;
}
int64_t fa_monotonic_ns(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (int64_t)((double)now.QuadPart * 1e9 / (double)freq.QuadPart);
}

#else
#  include <errno.h>
#  include <pthread.h>
#  include <time.h>
#  include <unistd.h>

int fa_mutex_init(fa_mutex* m)
{
    pthread_mutex_t* l = (pthread_mutex_t*)malloc(sizeof(pthread_mutex_t));
    if (!l) return -1;
    if (pthread_mutex_init(l, NULL) != 0) { free(l); return -1; }
    m->impl = l;
    return 0;
}
void fa_mutex_destroy(fa_mutex* m)
{
    if (!m->impl) return;
    pthread_mutex_destroy((pthread_mutex_t*)m->impl);
    free(m->impl);
    m->impl = NULL;
}
void fa_mutex_lock(fa_mutex* m) { pthread_mutex_lock((pthread_mutex_t*)m->impl); }
int fa_mutex_trylock(fa_mutex* m) { return pthread_mutex_trylock((pthread_mutex_t*)m->impl) == 0; }
void fa_mutex_unlock(fa_mutex* m) { pthread_mutex_unlock((pthread_mutex_t*)m->impl); }

int fa_cond_init(fa_cond* c)
{
    pthread_cond_t* cv = (pthread_cond_t*)malloc(sizeof(pthread_cond_t));
    if (!cv) return -1;
    if (pthread_cond_init(cv, NULL) != 0) { free(cv); return -1; }
    c->impl = cv;
    return 0;
}
void fa_cond_destroy(fa_cond* c)
{
    if (!c->impl) return;
    pthread_cond_destroy((pthread_cond_t*)c->impl);
    free(c->impl);
    c->impl = NULL;
}
void fa_cond_wait_ms(fa_cond* c, fa_mutex* m, uint32_t timeout_ms)
{
    /* Wall-clock deadline: a clock jump only causes an early or late wakeup,
     * which every caller tolerates (they all re-check state in a loop). */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_cond_timedwait((pthread_cond_t*)c->impl, (pthread_mutex_t*)m->impl, &ts);
}
void fa_cond_broadcast(fa_cond* c) { pthread_cond_broadcast((pthread_cond_t*)c->impl); }

typedef struct { fa_thread_proc proc; void* arg; } thread_start;

static void* thread_main(void* p)
{
    thread_start s = *(thread_start*)p;
    free(p);
    s.proc(s.arg);
    return NULL;
}

int fa_thread_create(fa_thread* t, fa_thread_proc proc, void* arg)
{
    pthread_t* th = (pthread_t*)malloc(sizeof(pthread_t));
    thread_start* s = (thread_start*)malloc(sizeof(*s));
    if (!th || !s) { free(th); free(s); return -1; }
    s->proc = proc;
    s->arg = arg;
    if (pthread_create(th, NULL, thread_main, s) != 0) { free(th); free(s); return -1; }
    t->impl = th;
    return 0;
}
void fa_thread_join(fa_thread* t)
{
    if (!t->impl) return;
    pthread_join(*(pthread_t*)t->impl, NULL);
    free(t->impl);
    t->impl = NULL;
}
void fa_thread_detach(fa_thread* t)
{
    if (!t->impl) return;
    pthread_detach(*(pthread_t*)t->impl);
    free(t->impl);
    t->impl = NULL;
}
uintptr_t fa_thread_current_id(void) { return (uintptr_t)pthread_self(); }

void fa_sleep_ms(uint32_t ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) { }
}
uint32_t fa_cpu_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (uint32_t)n : 1;
}
int64_t fa_monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
#endif
