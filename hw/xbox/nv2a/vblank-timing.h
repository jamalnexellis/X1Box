/* Deadline arithmetic for the optional phase-preserving VBlank cadence. */
#ifndef NV2A_VBLANK_TIMING_H
#define NV2A_VBLANK_TIMING_H

#include <assert.h>
#include <stdint.h>

/* Advance to the first deadline after now, skipping missed periods rather
 * than firing a burst or moving the clock's phase to the callback time.
 * QEMU_CLOCK_REALTIME and its scheduled deadlines are nonnegative. */
static inline int64_t nv2a_vblank_next_regular(int64_t target, int64_t now,
                                             int64_t period)
{
    assert(target >= 0 && now >= 0 && period > 0);
    if (target > INT64_MAX - period) {
        return INT64_MAX;
    }
    int64_t next = target + period;
    if (next > now) {
        return next;
    }
    uint64_t steps = (uint64_t)(now - next) / (uint64_t)period + 1;
    if (steps > (uint64_t)(INT64_MAX - next) / (uint64_t)period) {
        return INT64_MAX;
    }
    return next + (int64_t)(steps * (uint64_t)period);
}

#endif
