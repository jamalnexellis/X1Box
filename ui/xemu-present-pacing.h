/* Bound native scanout requests independently of SDL input polling. */
#ifndef XEMU_PRESENT_PACING_H
#define XEMU_PRESENT_PACING_H

#include <stdbool.h>
#include <stdint.h>

static inline bool xemu_present_request_due(int64_t last_request_ns,
                                            int64_t now_ns,
                                            int64_t interval_ns)
{
    /* A slow request never creates a backlog of catch-up presentations. */
    return last_request_ns < 0 || now_ns < last_request_ns ||
           now_ns - last_request_ns >= interval_ns;
}

#endif
