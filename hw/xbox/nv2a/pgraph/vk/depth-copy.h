/* Only the bit-preserving D16 depth -> R16 color route can bypass host reuse. */
#ifndef XEMU_VK_DEPTH_COPY_H
#define XEMU_VK_DEPTH_COPY_H
#include <stdbool.h>
#include <stdint.h>
#include "scaled-copy.h"
typedef struct VkDepthCopyPolicy {
    uint32_t width, height, scale, depth, dimensions, levels, layers;
    uint32_t source_width, source_height, source_image_width, source_image_height;
    uint32_t destination_width, destination_height;
    uint64_t scratch_capacity;
    bool allow_scaled, cubemap, border, active, initialized;
    bool upload_pending, download_pending, native_d16_r16, distinct_images;
} VkDepthCopyPolicy;

static inline bool vk_depth_copy_eligible(const VkDepthCopyPolicy *p)
{
    uint32_t width, height;
    return vk_scaled_copy_extent(p->width, p->height, p->scale, p->allow_scaled,
                                 &width, &height) &&
        p->width == p->source_width && p->height == p->source_height &&
        width == p->source_image_width && height == p->source_image_height &&
        width == p->destination_width && height == p->destination_height &&
        p->scratch_capacity >= (uint64_t)width * height * 2 &&
        p->depth == 1 && p->dimensions == 2 && p->levels == 1 && p->layers == 1 &&
        !p->cubemap && !p->border && p->active && p->initialized &&
        !p->upload_pending && !p->download_pending &&
        p->native_d16_r16 && p->distinct_images;
}
#endif
