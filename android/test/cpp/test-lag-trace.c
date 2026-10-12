#include <assert.h>
#include <string.h>
#include "hw/xbox/nv2a/pgraph/lag-trace-buffer.h"

int main(void)
{
    NV2ALagEvent storage[3];
    memset(storage, 0x5a, sizeof(storage));
    NV2ALagBuffer b = { .events = storage, .capacity = 2,
                       .begin_us = 100, .deadline_us = 200 };
    NV2ALagEvent e = { .id = 77, .link = 13, .start_us = 90,
                       .end_us = 99, .tid = 42 };
    assert(!nv2a_lag_buffer_append(&b, e));
    e.end_us = 100;
    assert(nv2a_lag_buffer_append(&b, e));
    e.end_us = 200;
    assert(nv2a_lag_buffer_append(&b, e));
    assert(storage[0].seq == 1 && storage[1].seq == 2);
    assert(storage[0].id == 77 && storage[0].link == 13);
    assert(storage[0].start_us == 90 && storage[0].tid == 42);
    assert(!nv2a_lag_buffer_append(&b, e) && b.dropped == 1);
    e.end_us = 201;
    assert(!nv2a_lag_buffer_append(&b, e) && b.dropped == 1);
    const unsigned char *guard = (const unsigned char *)&storage[2];
    for (size_t i = 0; i < sizeof(storage[2]); ++i) { assert(guard[i] == 0x5a); }
    b.events = NULL;
    e.end_us = 150;
    assert(!nv2a_lag_buffer_append(&b, e));
    for (unsigned bits = 36; bits <= 64; ++bits) {
        uint64_t mask = bits == 64 ? UINT64_MAX : (UINT64_C(1) << bits) - 1;
        for (uint64_t delta = 0; delta < 4096; ++delta) {
            uint64_t begin = mask - 100;
            uint64_t end = (begin + delta) & mask;
            assert(nv2a_lag_tick_delta(begin, end, bits) == delta);
        }
    }
    assert(nv2a_lag_tick_delta(0, 1, 0) == 0);
    assert(nv2a_lag_tick_delta(0, 1, 65) == 0);
    return 0;
}
