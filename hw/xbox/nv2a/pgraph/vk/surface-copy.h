/* Narrow eligibility for an in-place GPU-only color surface refresh. */
#ifndef XEMU_VK_SURFACE_COPY_H
#define XEMU_VK_SURFACE_COPY_H
#include <stdbool.h>
#include <stdint.h>
#include "scaled-copy.h"

typedef struct VkSurfaceCopyPolicy {
    uint32_t width, height, depth, dimensions, levels, layers, scale;
    uint32_t source_width, source_height;
    uint32_t source_image_width, source_image_height;
    uint32_t destination_width, destination_height;
    bool allow_scaled;
    bool cubemap, border, color, active, initialized;
    bool upload_pending, download_pending, byte_copy_compatible, distinct_images;
} VkSurfaceCopyPolicy;

static inline bool vk_surface_copy_storage_border(bool border_flag, bool linear)
{
    /* Linear textures retain their explicit rectangle. Only non-linear
     * textures use this flag to add a virtual border to image storage. */
    return border_flag && !linear;
}

enum VkSurfaceCopyRejection {
    VK_SURFACE_COPY_GEOMETRY = 1,
    VK_SURFACE_COPY_BORDER = 2,
    VK_SURFACE_COPY_SOURCE = 4,
    VK_SURFACE_COPY_PENDING = 8,
    VK_SURFACE_COPY_FORMAT_ALLOCATION = 16,
    VK_SURFACE_COPY_IMAGES = 32,
    VK_SURFACE_COPY_LAYOUT = 64,
};

static inline uint32_t vk_surface_copy_rejections(const VkSurfaceCopyPolicy *p)
{
    uint32_t reasons = 0;
    uint32_t width = 0, height = 0;
    bool extent_valid = vk_scaled_copy_extent(p->width, p->height, p->scale,
        p->allow_scaled, &width, &height);
    if (!extent_valid ||
        p->width != p->source_width || p->height != p->source_height ||
        p->depth != 1 || p->dimensions != 2 || p->levels != 1 ||
        p->layers != 1 || p->cubemap) {
        reasons |= VK_SURFACE_COPY_GEOMETRY;
    }
    if (extent_valid && (p->source_image_width != width ||
        p->source_image_height != height || p->destination_width != width ||
        p->destination_height != height)) {
        reasons |= VK_SURFACE_COPY_FORMAT_ALLOCATION;
    }
    if (p->border) {
        reasons |= VK_SURFACE_COPY_BORDER;
    }
    if (!p->color || !p->active || !p->initialized) {
        reasons |= VK_SURFACE_COPY_SOURCE;
    }
    if (p->upload_pending || p->download_pending) {
        reasons |= VK_SURFACE_COPY_PENDING;
    }
    if (!p->byte_copy_compatible) {
        reasons |= VK_SURFACE_COPY_FORMAT_ALLOCATION;
    }
    if (!p->distinct_images) {
        reasons |= VK_SURFACE_COPY_IMAGES;
    }
    return reasons;
}

static inline bool vk_surface_copy_eligible(const VkSurfaceCopyPolicy *p)
{
    return vk_surface_copy_rejections(p) == 0;
}
#endif
