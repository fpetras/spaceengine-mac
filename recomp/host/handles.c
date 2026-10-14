#include <pthread.h>

#include "handles.h"
#include "host.h"

enum { COUNT = 1024, BASE = 0x100 };
static struct { int kind; void *data; } table[COUNT];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

uint64_t handle_alloc(int kind, void *data) {
    uint64_t h = INVALID_HANDLE;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < COUNT; i++)
        if (table[i].kind == HK_FREE) {
            table[i].kind = kind;
            table[i].data = data;
            h = BASE + (uint64_t)i * 4;
            break;
        }
    pthread_mutex_unlock(&lock);
    return h;
}

static int index_of(uint64_t h) {
    uint64_t i = (h - BASE) / 4;
    return h >= BASE && i < COUNT && (h - BASE) % 4 == 0 ? (int)i : -1;
}

int handle_kind(uint64_t h) {
    int i = index_of(h);
    return i < 0 ? HK_FREE : table[i].kind;
}

void *handle_data(uint64_t h, int kind) {
    int i = index_of(h);
    return i >= 0 && table[i].kind == kind ? table[i].data : 0;
}

void handle_release(uint64_t h) {
    int i = index_of(h);
    if (i >= 0) table[i].kind = HK_FREE;
}
