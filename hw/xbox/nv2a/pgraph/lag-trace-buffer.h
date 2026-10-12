/* Storage only; caller serializes append/detach. No allocation or I/O here. */
#ifndef NV2A_LAG_TRACE_BUFFER_H
#define NV2A_LAG_TRACE_BUFFER_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef struct NV2ALagEvent {
    uint64_t seq, id, link, frame, a, b, c;
    int64_t start_us, end_us;
    uint32_t tid, kind;
} NV2ALagEvent;

typedef struct NV2ALagBuffer {
    NV2ALagEvent *events;
    size_t count, capacity;
    uint64_t dropped;
    int64_t begin_us, deadline_us;
} NV2ALagBuffer;

static inline bool nv2a_lag_buffer_append(NV2ALagBuffer *buffer,
                                         NV2ALagEvent event)
{
    if (!buffer->events || event.end_us < buffer->begin_us ||
        event.end_us > buffer->deadline_us) {
        return false;
    }
    if (buffer->count == buffer->capacity) {
        buffer->dropped++;
        return false;
    }
    event.seq = buffer->count + 1;
    buffer->events[buffer->count++] = event;
    return true;
}

/* The selected Vulkan queue supplies valid bits. Unsigned subtraction handles
 * wrap; never subtract truncated timestamps as signed integers. */
static inline uint64_t nv2a_lag_tick_delta(uint64_t begin, uint64_t end,
                                          unsigned valid_bits)
{
    if (!valid_bits || valid_bits > 64) {
        return 0;
    }
    uint64_t mask = valid_bits == 64 ? UINT64_MAX :
                    (UINT64_C(1) << valid_bits) - 1;
    return (end - begin) & mask;
}
#endif
