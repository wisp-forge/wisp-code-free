#ifndef THREADS_H
#define THREADS_H

#if defined(_WIN32)

#include <windows.h>
#include <process.h>
#include <stdlib.h>
#include <stdint.h>

typedef SRWLOCK pthread_mutex_t;
#define PTHREAD_MUTEX_INITIALIZER SRWLOCK_INIT

static inline int pthread_mutex_init(pthread_mutex_t *m, const void *attr) {
    (void)attr;
    InitializeSRWLock(m);
    return 0;
}
static inline int pthread_mutex_lock(pthread_mutex_t *m) {
    AcquireSRWLockExclusive(m);
    return 0;
}
static inline int pthread_mutex_unlock(pthread_mutex_t *m) {
    ReleaseSRWLockExclusive(m);
    return 0;
}
static inline int pthread_mutex_destroy(pthread_mutex_t *m) {
    (void)m;
    return 0;
}

typedef CONDITION_VARIABLE pthread_cond_t;
#define PTHREAD_COND_INITIALIZER CONDITION_VARIABLE_INIT

static inline int pthread_cond_init(pthread_cond_t *c, const void *attr) {
    (void)attr;
    InitializeConditionVariable(c);
    return 0;
}
static inline int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    return SleepConditionVariableSRW(c, m, INFINITE, 0) ? 0 : -1;
}
static inline int pthread_cond_signal(pthread_cond_t *c) {
    WakeConditionVariable(c);
    return 0;
}
static inline int pthread_cond_broadcast(pthread_cond_t *c) {
    WakeAllConditionVariable(c);
    return 0;
}
static inline int pthread_cond_destroy(pthread_cond_t *c) {
    (void)c;
    return 0;
}

typedef HANDLE pthread_t;

typedef struct {
    void *(*fn)(void *);
    void  *arg;
} wisp_thread_trampoline_t;

static unsigned __stdcall wisp_thread_entry(void *p) {
    wisp_thread_trampoline_t *t = (wisp_thread_trampoline_t *)p;
    void *(*fn)(void *) = t->fn;
    void *arg = t->arg;
    free(t);
    fn(arg);
    return 0;
}

static inline int pthread_create(pthread_t *out, const void *attr,
                                 void *(*fn)(void *), void *arg) {
    (void)attr;
    wisp_thread_trampoline_t *t = malloc(sizeof(*t));
    if (!t) return -1;
    t->fn  = fn;
    t->arg = arg;

    uintptr_t h = _beginthreadex(NULL, 0, wisp_thread_entry, t, 0, NULL);
    if (h == 0) { free(t); return -1; }
    *out = (HANDLE)h;
    return 0;
}

static inline int pthread_detach(pthread_t t) {
    CloseHandle(t);
    return 0;
}

static inline int pthread_join(pthread_t t, void **retval) {
    (void)retval;
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    return 0;
}

#else
#include <pthread.h>
#endif

#endif /* WISP_THREADS_H */