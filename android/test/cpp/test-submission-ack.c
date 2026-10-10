/* Exercise the production handshake with POSIX-backed QEMU API adapters.
 * This tests wakeup ordering, not Vulkan execution or Android scheduling. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

typedef pthread_mutex_t QemuMutex;
typedef pthread_cond_t QemuCond;
#define qemu_mutex_lock(lock) assert(pthread_mutex_lock(lock) == 0)
#define qemu_mutex_unlock(lock) assert(pthread_mutex_unlock(lock) == 0)
#define qemu_cond_wait(cond, lock) assert(pthread_cond_wait(cond, lock) == 0)
#define qemu_cond_broadcast(cond) assert(pthread_cond_broadcast(cond) == 0)
#define qatomic_read(ptr) __atomic_load_n(ptr, __ATOMIC_RELAXED)
#define qatomic_set(ptr, value) __atomic_store_n(ptr, value, __ATOMIC_RELAXED)
#include "hw/xbox/nv2a/pgraph/vk/submission-ack.h"

typedef struct Waiter {
    QemuMutex *lock;
    QemuCond *condition;
    bool *submitted;
    bool started, returned;
} Waiter;

static void pause_briefly(void)
{
    struct timespec duration = { .tv_nsec = 1000000 };
    assert(nanosleep(&duration, NULL) == 0);
}

static void *waiter_main(void *opaque)
{
    Waiter *waiter = opaque;
    qatomic_set(&waiter->started, true);
    vk_submission_ack_wait(waiter->lock, waiter->condition, waiter->submitted);
    qatomic_set(&waiter->returned, true);
    return NULL;
}

int main(void)
{
    alarm(20); /* Turn a lost wakeup into a bounded test failure. */
    QemuMutex lock = PTHREAD_MUTEX_INITIALIZER;
    QemuCond condition = PTHREAD_COND_INITIALIZER;
    bool submitted[2] = { false, false };

    /* A notification before the waiter starts must not be lost. */
    vk_submission_ack_publish(&lock, &condition, &submitted[0]);
    vk_submission_ack_wait(&lock, &condition, &submitted[0]);

    for (unsigned int repetition = 0; repetition < 100; repetition++) {
        Waiter waiters[2] = {
            { .lock = &lock, .condition = &condition,
              .submitted = &submitted[0] },
            { .lock = &lock, .condition = &condition,
              .submitted = &submitted[1] },
        };
        pthread_t threads[2];
        for (int i = 0; i < 2; i++) {
            qatomic_set(&submitted[i], false);
            assert(pthread_create(&threads[i], NULL, waiter_main,
                                  &waiters[i]) == 0);
        }
        while (!qatomic_read(&waiters[0].started) ||
               !qatomic_read(&waiters[1].started)) {
            pause_briefly();
        }

        /* A spurious broadcast must not acknowledge either slot. */
        qemu_mutex_lock(&lock);
        qemu_cond_broadcast(&condition);
        qemu_mutex_unlock(&lock);
        pause_briefly();
        assert(!qatomic_read(&waiters[0].returned));
        assert(!qatomic_read(&waiters[1].returned));

        /* Acknowledging slot 1 must not release slot 0. */
        vk_submission_ack_publish(&lock, &condition, &submitted[1]);
        assert(pthread_join(threads[1], NULL) == 0);
        assert(qatomic_read(&waiters[1].returned));
        assert(!qatomic_read(&waiters[0].returned));
        vk_submission_ack_publish(&lock, &condition, &submitted[0]);
        assert(pthread_join(threads[0], NULL) == 0);
        assert(qatomic_read(&waiters[0].returned));
    }
    assert(pthread_cond_destroy(&condition) == 0);
    assert(pthread_mutex_destroy(&lock) == 0);
    alarm(0);
    puts("Submission acknowledgement ordering checks passed");
    return 0;
}
