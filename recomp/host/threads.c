// Threads, waiting and condition variables.
//
// Each thread of the game is a host thread with its own translated CPU state,
// stack, thread block and thread-local storage.
#include <errno.h>
#include <os/lock.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "handles.h"
#include "host.h"

enum { WAIT_OBJECT_0 = 0, WAIT_TIMEOUT = 258, WAIT_FAILED = 0xffffffffu, STILL_ACTIVE = 259, ERROR_TIMEOUT = 1460 };
#define INFINITE 0xffffffffu

typedef struct {
    pthread_t thread;
    uint64_t start, parameter;
    uint32_t id, exit_code;
    int finished, handle_open;
    pthread_mutex_t lock;
    pthread_cond_t done;
} thread_t;

static _Thread_local thread_t *current;

static struct timespec deadline_after(uint32_t ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
    return ts;
}

static void finish(thread_t *t, uint32_t code) {
    pthread_mutex_lock(&t->lock);
    t->exit_code = code;
    t->finished = 1;
    pthread_cond_broadcast(&t->done);
    pthread_mutex_unlock(&t->lock);
}

// Windows runs the image's TLS callbacks when a thread starts; the C runtime
// uses one to construct thread_local objects.
void host_run_tls_callbacks(cpu_t *c, uint32_t reason) {
    const uint64_t base = 0x140000000ull;
    uint32_t tls = ld32(base + ld32(base + 0x3c) + 24 + 112 + 9 * 8);
    uint64_t list = tls ? ld64(base + tls + 24) : 0;
    for (; list && ld64(list); list += 8) {
        recomp_fn fn = recomp_lookup(ld64(list));
        if (!fn) continue;
        c->r[RCX] = base; c->r[RDX] = reason; c->r[R8] = 0;
        recomp_invoke(c, fn);
    }
}

static void *thread_main(void *arg) {
    thread_t *t = arg;
    current = t;
    cpu_t c;
    memset(&c, 0, sizeof c);
    host_thread.id = t->id;
    host_thread_attach(&c);
    host_run_tls_callbacks(&c, 2);  // DLL_THREAD_ATTACH
    recomp_fn fn = recomp_lookup(t->start);
    if (!fn) { fprintf(stderr, "host: thread start routine %#llx is not translated\n", (unsigned long long)t->start); exit(70); }
    c.r[RCX] = t->parameter;
    recomp_invoke(&c, fn);
    finish(t, (uint32_t)c.r[RAX]);
    return NULL;
}

IMPORT(CreateThread) {
    thread_t *t = calloc(1, sizeof *t);
    t->start = ARG(2);
    t->parameter = ARG(3);
    t->handle_open = 1;
    t->id = host_new_thread_id();
    pthread_mutex_init(&t->lock, NULL);
    pthread_cond_init(&t->done, NULL);
    uint64_t h = handle_alloc(HK_THREAD, t);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (ARG(5)) st32(ARG(5), t->id);  // std::thread treats an ID of 0 as "not a thread" and refuses to join it
    if (pthread_create(&t->thread, &attr, thread_main, t) != 0) { handle_release(h); free(t); RET(0); }
    RET(h);
}
IMPORT(ExitThread) {
    if (current) finish(current, (uint32_t)ARG(0));
    pthread_exit(NULL);
}
IMPORT(FreeLibraryAndExitThread) {
    if (current) finish(current, (uint32_t)ARG(1));
    pthread_exit(NULL);
}
IMPORT(GetExitCodeThread) {
    thread_t *t = handle_data(ARG(0), HK_THREAD);
    if (!t) RET(0);
    pthread_mutex_lock(&t->lock);
    st32(ARG(1), t->finished ? t->exit_code : STILL_ACTIVE);
    pthread_mutex_unlock(&t->lock);
    RET(1);
}
static uint32_t wait_for(uint64_t handle, uint32_t ms) {
    thread_t *t = handle_data(handle, HK_THREAD);
    if (!t) {
        fprintf(stderr, "host: wait on a handle that is not a thread (%#llx)\n", (unsigned long long)handle);
        return WAIT_FAILED;
    }
    uint32_t r = WAIT_OBJECT_0;
    struct timespec until = deadline_after(ms == INFINITE ? 0 : ms);
    pthread_mutex_lock(&t->lock);
    while (!t->finished) {
        if (ms == INFINITE) pthread_cond_wait(&t->done, &t->lock);
        else if (pthread_cond_timedwait(&t->done, &t->lock, &until) == ETIMEDOUT) { r = WAIT_TIMEOUT; break; }
    }
    pthread_mutex_unlock(&t->lock);
    return r;
}
IMPORT(WaitForSingleObject) { RET(wait_for(ARG(0), (uint32_t)ARG(1))); }
IMPORT(WaitForSingleObjectEx) { RET(wait_for(ARG(0), (uint32_t)ARG(1))); }
void host_thread_handle_closed(uint64_t handle) {
    // The thread object is kept: it is small, and the thread may still be running.
    handle_release(handle);
}
IMPORT(OpenEventW) { SET_ERROR(2); RET(0); }  // no named events exist

// ----------------------------------------------------- condition variables
// A CONDITION_VARIABLE is one zero-initialised pointer with no init call, so
// the waiting is done on host condition variables chosen by its address.
// Waking more waiters than asked is allowed: Windows permits spurious wakeups.
enum { BUCKETS = 64 };
static struct { pthread_mutex_t lock; pthread_cond_t cond; } buckets[BUCKETS];
static pthread_once_t buckets_once = PTHREAD_ONCE_INIT;
static void buckets_init(void) {
    for (int i = 0; i < BUCKETS; i++) { pthread_mutex_init(&buckets[i].lock, NULL); pthread_cond_init(&buckets[i].cond, NULL); }
}
static int bucket_of(uint64_t address) { return (int)((address >> 3) * 0x9e3779b97f4a7c15ull >> 58); }

IMPORT(SleepConditionVariableSRW) {
    pthread_once(&buckets_once, buckets_init);
    int b = bucket_of(ARG(0));
    uint32_t ms = (uint32_t)ARG(2);
    struct timespec until = deadline_after(ms == INFINITE ? 0 : ms);
    int timed_out = 0;
    pthread_mutex_lock(&buckets[b].lock);
    os_unfair_lock_unlock((os_unfair_lock *)PTR(1));   // released only once this thread cannot miss a wake
    if (ms == INFINITE) pthread_cond_wait(&buckets[b].cond, &buckets[b].lock);
    else timed_out = pthread_cond_timedwait(&buckets[b].cond, &buckets[b].lock, &until) == ETIMEDOUT;
    pthread_mutex_unlock(&buckets[b].lock);
    os_unfair_lock_lock((os_unfair_lock *)PTR(1));
    if (timed_out) SET_ERROR(ERROR_TIMEOUT);
    RET(!timed_out);
}
static void wake(uint64_t address) {
    pthread_once(&buckets_once, buckets_init);
    int b = bucket_of(address);
    pthread_mutex_lock(&buckets[b].lock);
    pthread_cond_broadcast(&buckets[b].cond);
    pthread_mutex_unlock(&buckets[b].lock);
}
IMPORT(WakeAllConditionVariable) { wake(ARG(0)); }
IMPORT(WakeConditionVariable) { wake(ARG(0)); }
