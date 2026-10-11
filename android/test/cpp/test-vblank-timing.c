/* Check cadence against an independently stepped deadline clock. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "hw/xbox/nv2a/vblank-timing.h"

static void check_cadence(int64_t period)
{
    const int64_t origin = 123456789;
    int64_t target = origin;
    uint32_t random = 0x72835u;
    for (unsigned i = 0; i < 10000; i++) {
        random = random * 1664525u + 1013904223u;
        int64_t now = target + random % (period * 12);
        int64_t expected = target;
        do {
            expected += period;
        } while (expected <= now);
        target = nv2a_vblank_next_regular(target, now, period);
        assert(target == expected);
        assert(target > now && target - now <= period);
        assert((target - origin) % period == 0);
    }
}

int main(void)
{
    check_cadence(16683750); /* Existing NTSC period. */
    check_cadence(20000000); /* Existing PAL period. */
    check_cadence(1);
    assert(nv2a_vblank_next_regular(100, 100, 10) == 110);
    assert(nv2a_vblank_next_regular(100, 109, 10) == 110);
    assert(nv2a_vblank_next_regular(100, 110, 10) == 120);
    assert(nv2a_vblank_next_regular(100, 139, 10) == 140);
    /* Even a very late callback produces a single future deadline. */
    assert(nv2a_vblank_next_regular(100, 1000000000, 10) == 1000000010);
    /* A mode change can shorten the period before the old target. */
    assert(nv2a_vblank_next_regular(200, 100, 10) == 210);
    assert(nv2a_vblank_next_regular(INT64_MAX - 11, INT64_MAX - 2, 10) == INT64_MAX - 1);
    assert(nv2a_vblank_next_regular(INT64_MAX - 9, INT64_MAX - 1, 10) == INT64_MAX);
    assert(nv2a_vblank_next_regular(0, INT64_MAX, 1) == INT64_MAX);
    assert(nv2a_vblank_next_regular(0, INT64_MAX - 1, INT64_MAX) == INT64_MAX);
    puts("Regular VBlank cadence: 30,000 stepped callbacks and boundaries passed");
    return 0;
}
