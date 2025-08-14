#ifndef THREAD_H_INCLUDED
#define THREAD_H_INCLUDED

#include <windows.h>
#include <stdlib.h>

typedef HANDLE pthread_t;

/* ====== barrier ====== */
typedef struct {
    unsigned total;
    unsigned count;
    unsigned generation;
    CRITICAL_SECTION lock;
    HANDLE event;
} pthread_barrier_t;

static int pthread_barrier_init(pthread_barrier_t *b, void *attr, unsigned count) {
    (void)attr;
    if (!b || count == 0) return -1;
    InitializeCriticalSection(&b->lock);
    b->event = CreateEvent(NULL, TRUE, FALSE, NULL); /* manual reset */
    if (!b->event) return -1;
    b->total = count;
    b->count = 0;
    b->generation = 0;
    return 0;
}

static int pthread_barrier_wait(pthread_barrier_t *b) {
    EnterCriticalSection(&b->lock);
    unsigned gen = b->generation;
    if (++b->count == b->total) {
        b->generation++;
        b->count = 0;
        SetEvent(b->event);
        ResetEvent(b->event);
        LeaveCriticalSection(&b->lock);
        return 0;
    }
    LeaveCriticalSection(&b->lock);
    WaitForSingleObject(b->event, INFINITE);
    return 0;
}

static int pthread_barrier_destroy(pthread_barrier_t *b) {
    DeleteCriticalSection(&b->lock);
    CloseHandle(b->event);
    return 0;
}

/* ====== threads ====== */
typedef void *(*_pt_fn)(void *);
struct _pt_pack { _pt_fn fn; void *arg; };

static DWORD WINAPI _pt_thunk(LPVOID p) {
    struct _pt_pack *pk = (struct _pt_pack*)p;
    _pt_fn fn = pk->fn; void *arg = pk->arg;
    free(pk);
    (void)fn(arg);
    return 0;
}

static int pthread_create(pthread_t *t, void *attr, void *(*start_routine)(void*), void *arg) {
    (void)attr;
    if (!t || !start_routine) return -1;
    struct _pt_pack *pk = (struct _pt_pack*)malloc(sizeof *pk);
    if (!pk) return -1;
    pk->fn = start_routine; pk->arg = arg;
    HANDLE h = CreateThread(NULL, 0, _pt_thunk, pk, 0, NULL);
    if (!h) { free(pk); return -1; }
    *t = h;
    return 0;
}

static int pthread_join(pthread_t t, void **ret) {
    (void)ret;
    if (!t) return -1;
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    return 0;
}

static int pthread_detach(pthread_t t) {
    if (!t) return -1;
    CloseHandle(t);
    return 0;
}

#endif
