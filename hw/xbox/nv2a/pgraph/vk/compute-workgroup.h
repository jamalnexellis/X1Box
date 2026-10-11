#ifndef XEMU_VK_COMPUTE_WORKGROUP_H
#define XEMU_VK_COMPUTE_WORKGROUP_H
#include <stdint.h>

static inline uint32_t vk_compute_workgroup_size(uint32_t units,
    uint32_t max_x, uint32_t max_invocations)
{
    if (!units || !max_x || !max_invocations) return 0;
    uint32_t size = 1024;
    while (size > max_x || size > max_invocations || units % size) size /= 2;
    return size;
}
#endif
