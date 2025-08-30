#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef HANDLE thread;

struct _th_pack { void *(*fn)(void *); void *arg; };

static DWORD WINAPI _th_win_thunk(LPVOID q) {
    struct _th_pack *p = (struct _th_pack*)q;
    void *(*f)(void*) = p->fn; void *a = p->arg;
    free(p);
    f(a);
    return 0;
}

static int thread_create(thread *t, void *(*fn)(void*), void *arg) {
    if (!t || !fn) return -1;
    struct _th_pack *p = (struct _th_pack*)malloc(sizeof *p);
    if (!p) return -1;
    p->fn = fn; p->arg = arg;
    HANDLE h = CreateThread(NULL, 0, _th_win_thunk, p, 0, NULL);
    if (!h) { free(p); return -1; }
    *t = h; return 0;
}

static int thread_join(thread t, void **ret) { (void)ret; if (!t) return -1; WaitForSingleObject(t, INFINITE); CloseHandle(t); return 0; }
static int thread_detach(thread t) { if (!t) return -1; CloseHandle(t); return 0; }
static void thread_sleep_ms(unsigned ms) { Sleep(ms); }

typedef struct {
    unsigned total;
    unsigned count;
    unsigned generation;
    CRITICAL_SECTION lock;
    HANDLE event;
} barrier;

static int barrier_init(barrier *b, unsigned count) {
    if (!b || count == 0) return -1;
    InitializeCriticalSection(&b->lock);
    b->event = CreateEvent(NULL, TRUE, FALSE, NULL); /* manual reset */
    if (!b->event) return -1;
    b->total = count;
    b->count = 0;
    b->generation = 0;
    return 0;
}

static int barrier_wait(barrier *b) {
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

static int barrier_destroy(barrier *b) {
    DeleteCriticalSection(&b->lock);
    CloseHandle(b->event);
    return 0;
}
#else /* POSIX */

#include <pthread.h>
#include <time.h>
#include <unistd.h>

typedef pthread_t thread;

static int thread_create(thread *t, void *(*fn)(void*), void *arg) { return (!t || !fn) ? -1 : pthread_create(t, NULL, fn, arg); }
static int thread_join(thread t, void **ret) { return pthread_join(t, ret); }
static int thread_detach(thread t) { return pthread_detach(t); }
static void thread_sleep_ms(unsigned ms) { struct timespec ts = { ms/1000, (long)(ms%1000)*1000000L }; nanosleep(&ts, NULL); }

/* Prefer native pthread barrier if available (Linux glibc, musl: yes). */
#ifdef PTHREAD_BARRIER_SERIAL_THREAD
typedef barrier barrier;
static int barrier_init(barrier *b, unsigned n) { return (!b || n==0) ? -1 : pthread_barrier_init(b, NULL, n); }
static int barrier_wait(barrier *b) { int r = pthread_barrier_wait(b); return (r == PTHREAD_BARRIER_SERIAL_THREAD) ? 1 : 0; }
static int barrier_destroy(barrier *b) { return pthread_barrier_destroy(b); }

#else
/* Portable fallback (kept here only if you ever target libcs without barriers). */
typedef struct { unsigned total, count, generation; pthread_mutex_t m; pthread_cond_t c; } barrier;
static int barrier_init(barrier *b, unsigned n){ if(!b||!n) return -1; b->total=n;b->count=0;b->generation=0; if(pthread_mutex_init(&b->m,NULL))return -1; if(pthread_cond_init(&b->c,NULL)){pthread_mutex_destroy(&b->m);return -1;} return 0; }
static int barrier_wait(barrier *b){ if(pthread_mutex_lock(&b->m))return -1; unsigned gen=b->generation; if(++b->count==b->total){ b->generation++; b->count=0; pthread_cond_broadcast(&b->c); pthread_mutex_unlock(&b->m); return 1; } while(gen==b->generation) pthread_cond_wait(&b->c,&b->m); pthread_mutex_unlock(&b->m); return 0; }
static int barrier_destroy(barrier *b){ int e1=pthread_mutex_destroy(&b->m); int e2=pthread_cond_destroy(&b->c); return (e1||e2)?-1:0; }
#endif

#endif