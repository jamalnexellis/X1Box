/* Raw value equality without out-of-range or alignment-dependent reads. */
#ifndef NV2A_VK_UNIFORM_VALUE_COMPARE_H
#define NV2A_VK_UNIFORM_VALUE_COMPARE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline bool nv2a_vk_uniform_values_equal(const void *a, const void *b,
                                               size_t size)
{
    if (!size) {
        return true;
    }
    if (size == 4) {
        uint32_t av, bv;
        memcpy(&av, a, sizeof(av));
        memcpy(&bv, b, sizeof(bv));
        return av == bv;
    }
    if (size == 8 || size == 12 || size == 16) {
        uint64_t av, bv;
        memcpy(&av, a, sizeof(av));
        memcpy(&bv, b, sizeof(bv));
        if (av != bv) {
            return false;
        }
        if (size == 8) {
            return true;
        }
        if (size == 12) {
            uint32_t at, bt;
            memcpy(&at, (const uint8_t *)a + 8, sizeof(at));
            memcpy(&bt, (const uint8_t *)b + 8, sizeof(bt));
            return at == bt;
        }
        memcpy(&av, (const uint8_t *)a + 8, sizeof(av));
        memcpy(&bv, (const uint8_t *)b + 8, sizeof(bv));
        return av == bv;
    }
    return !memcmp(a, b, size);
}

#endif
