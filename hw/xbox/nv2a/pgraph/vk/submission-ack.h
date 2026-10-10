/* Submission acknowledgement only: GPU fence waits remain with their callers.
 * Include after QEMU's mutex, condition and atomic declarations. */
#ifndef XEMU_VK_SUBMISSION_ACK_H
#define XEMU_VK_SUBMISSION_ACK_H

static inline void vk_submission_ack_wait(QemuMutex *lock, QemuCond *condition,
                                         bool *submitted)
{
    qemu_mutex_lock(lock);
    while (!qatomic_read(submitted)) {
        qemu_cond_wait(condition, lock);
    }
    qemu_mutex_unlock(lock);
}

static inline void vk_submission_ack_publish(QemuMutex *lock,
                                            QemuCond *condition,
                                            bool *submitted)
{
    qemu_mutex_lock(lock);
    qatomic_set(submitted, true);
    qemu_cond_broadcast(condition);
    qemu_mutex_unlock(lock);
}

#endif
