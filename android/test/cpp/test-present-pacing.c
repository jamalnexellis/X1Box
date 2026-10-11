/* Timing regression checks for the Android native presentation experiment. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "ui/xemu-present-pacing.h"

int main(void)
{
    const int64_t interval = 1000000000LL / 60;
    int64_t last = -1;
    unsigned requests = 0;
    unsigned input_polls = 0;

    /* A 1 ms input loop must not become a 1,000 Hz scanout loop. */
    for (int64_t now = 0; now < 1000000000LL; now += 1000000) {
        if (xemu_present_request_due(last, now, interval)) {
            if (last >= 0) {
                assert(now - last >= interval);
            }
            last = now;
            requests++;
        }
        input_polls++;
    }
    assert(requests == 59);
    assert(input_polls == 1000);

    /* A blocked presentation returns late: request once, without a burst. */
    last = 0;
    assert(xemu_present_request_due(last, 60000000, interval));
    last = 60000000;
    assert(!xemu_present_request_due(last, 61000000, interval));
    assert(!xemu_present_request_due(last, last + interval - 1, interval));
    assert(xemu_present_request_due(last, last + interval, interval));

    /* Resume and clock discontinuities must not leave scanout suspended. */
    assert(xemu_present_request_due(-1, 61000000, interval));
    assert(xemu_present_request_due(last, last - 1, interval));

    /* Higher configured presentation rates remain supported. */
    assert(xemu_present_request_due(last, last + 1000000000LL / 120,
                                    1000000000LL / 120));
    puts("Native presentation pacing checks passed");
    return 0;
}
