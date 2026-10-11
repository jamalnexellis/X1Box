/* Exercise the production drain predicate with POSIX-backed QEMU adapters. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

typedef pthread_mutex_t QemuMutex;
typedef pthread_cond_t QemuCond;
#define qemu_mutex_lock(lock) assert(pthread_mutex_lock(lock) == 0)
#define qemu_mutex_unlock(lock) assert(pthread_mutex_unlock(lock) == 0)
static atomic_uint wait_entries;
static void qemu_cond_wait(QemuCond *cond, QemuMutex *lock)
{
    atomic_fetch_add(&wait_entries, 1);
    assert(pthread_cond_wait(cond, lock) == 0);
}
#include "hw/xbox/nv2a/pgraph/vk/render-idle.h"

typedef struct Worker {
    QemuMutex lock;
    QemuCond idle;
    int queued;
    bool active;
} Worker;
typedef struct Waiter {
    Worker *worker;
    atomic_bool returned;
} Waiter;

static void *wait_main(void *opaque)
{
    Waiter *waiter = opaque;
    Worker *w = waiter->worker;
    vk_render_wait_idle(&w->lock, &w->idle, &w->queued, &w->active);
    atomic_store(&waiter->returned, true);
    return NULL;
}

static void await_wait_entries(unsigned count)
{
    while (atomic_load(&wait_entries) < count) {
        struct timespec pause = { .tv_nsec = 100000 };
        assert(nanosleep(&pause, NULL) == 0);
    }
}

int main(void)
{
    alarm(20); /* A missed wakeup is a bounded failure. */
    Worker w = { .lock = PTHREAD_MUTEX_INITIALIZER,
                 .idle = PTHREAD_COND_INITIALIZER };
    /* Completion preceding any waiter must return without a notification. */
    vk_render_wait_idle(&w.lock, &w.idle, &w.queued, &w.active);

    for (unsigned repetition = 0; repetition < 200; repetition++) {
        w.queued = 0;
        w.active = true;
        /* Negative control: the former queue-only check would return here. */
        assert(w.queued == 0 && w.active);
        atomic_store(&wait_entries, 0);
        Waiter waiters[2] = { { .worker = &w }, { .worker = &w } };
        pthread_t threads[2];
        for (unsigned i = 0; i < 2; i++) {
            atomic_init(&waiters[i].returned, false);
            assert(pthread_create(&threads[i], NULL, wait_main,
                                  &waiters[i]) == 0);
        }
        await_wait_entries(2);
        qemu_mutex_lock(&w.lock);
        assert(!atomic_load(&waiters[0].returned));
        assert(!atomic_load(&waiters[1].returned));
        assert(pthread_cond_broadcast(&w.idle) == 0); /* Spurious wakeup. */
        qemu_mutex_unlock(&w.lock);
        await_wait_entries(4);

        /* Completing one command cannot release waiters with more queued. */
        qemu_mutex_lock(&w.lock);
        w.active = false;
        w.queued = 1;
        assert(pthread_cond_broadcast(&w.idle) == 0);
        qemu_mutex_unlock(&w.lock);
        await_wait_entries(6);
        qemu_mutex_lock(&w.lock);
        assert(!atomic_load(&waiters[0].returned));
        assert(!atomic_load(&waiters[1].returned));
        /* Last queued command is now active: empty still is not idle. */
        w.queued = 0;
        w.active = true;
        assert(pthread_cond_broadcast(&w.idle) == 0);
        qemu_mutex_unlock(&w.lock);
        await_wait_entries(8);
        qemu_mutex_lock(&w.lock);
        assert(!atomic_load(&waiters[0].returned));
        assert(!atomic_load(&waiters[1].returned));
        w.active = false;
        assert(pthread_cond_broadcast(&w.idle) == 0);
        qemu_mutex_unlock(&w.lock);
        for (unsigned i = 0; i < 2; i++) {
            assert(pthread_join(threads[i], NULL) == 0);
            assert(atomic_load(&waiters[i].returned));
        }
        vk_render_wait_idle(&w.lock, &w.idle, &w.queued, &w.active);
    }
    assert(pthread_cond_destroy(&w.idle) == 0);
    assert(pthread_mutex_destroy(&w.lock) == 0);
    alarm(0);
    puts("Render drain active-command and wakeup checks passed (200 rounds)");
    return 0;
}
