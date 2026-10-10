/* Narrow eligibility for an in-place GPU-only color surface refresh. */
#ifndef XEMU_VK_SURFACE_COPY_H
#define XEMU_VK_SURFACE_COPY_H
#include <stdbool.h>
#include <stdint.h>

typedef struct VkSurfaceCopyPolicy {
    uint32_t width, height, depth, dimensions, levels, layers, scale;
    uint32_t source_width, source_height;
    bool cubemap, border, color, active, initialized;
    bool upload_pending, download_pending, byte_copy_compatible, distinct_images;
} VkSurfaceCopyPolicy;

static inline bool vk_surface_copy_eligible(const VkSurfaceCopyPolicy *p)
{
    return p->width && p->height && p->width <= 4096 && p->height <= 4096 &&
        p->width == p->source_width && p->height == p->source_height &&
        p->depth == 1 && p->dimensions == 2 && p->levels == 1 &&
        p->layers == 1 && p->scale == 1 && !p->cubemap && !p->border &&
        p->color && p->active && p->initialized && !p->upload_pending &&
        !p->download_pending && p->byte_copy_compatible && p->distinct_images;
}
#endif
