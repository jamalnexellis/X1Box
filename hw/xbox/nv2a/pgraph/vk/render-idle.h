/* Caller and worker protect both predicates with the same mutex. */
#ifndef NV2A_VK_RENDER_IDLE_H
#define NV2A_VK_RENDER_IDLE_H

static inline void vk_render_wait_idle(QemuMutex *lock, QemuCond *condition,
                                      const int *queued, const bool *active)
{
    qemu_mutex_lock(lock);
    while (*queued || *active) {
        qemu_cond_wait(condition, lock);
    }
    qemu_mutex_unlock(lock);
}

#endif
