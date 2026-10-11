/* Strict image and buffer eligibility for bit-preserving depth copies. */
#ifndef XEMU_VK_DEPTH_COPY_H
#define XEMU_VK_DEPTH_COPY_H
#include <stdbool.h>
#include <stdint.h>
#include "scaled-copy.h"
#include "compute-workgroup.h"
typedef struct VkDepthCopyPolicy {
    uint32_t width, height, scale, depth, dimensions, levels, layers;
    uint32_t source_width, source_height, source_image_width, source_image_height;
    uint32_t destination_width, destination_height;
    uint64_t scratch_capacity;
    uint64_t output_capacity, stencil_alignment;
    uint32_t max_workgroup_x, max_invocations, max_group_count;
    bool allow_scaled, cubemap, border, active, initialized;
    bool upload_pending, download_pending, native_compatible, distinct_images;
    bool stencil;
} VkDepthCopyPolicy;

static inline uint64_t vk_depth_stencil_offset(uint32_t width, uint32_t height,
                                               uint64_t alignment)
{
    return ((uint64_t)width * height * 4 + alignment - 1) & ~(alignment - 1);
}

static inline bool vk_depth_copy_eligible(const VkDepthCopyPolicy *p)
{
    uint32_t width, height;
    if (!vk_scaled_copy_extent(p->width, p->height, p->scale, p->allow_scaled,
                               &width, &height)) return false;
    uint64_t pixels = (uint64_t)width * height;
    if (p->stencil) {
        /* The buffer shader reads stencil in groups of four bytes. Its
         * descriptor range must contain every word that it accesses. */
        if (pixels % 4 || !p->stencil_alignment ||
            p->stencil_alignment > p->scratch_capacity ||
            (p->stencil_alignment & (p->stencil_alignment - 1))) return false;
        uint64_t offset = vk_depth_stencil_offset(width, height, p->stencil_alignment);
        uint32_t group = vk_compute_workgroup_size(pixels, p->max_workgroup_x,
                                                    p->max_invocations);
        if (offset > p->scratch_capacity ||
            pixels > p->scratch_capacity - offset ||
            pixels * 4 > p->output_capacity || !group ||
            pixels / group > p->max_group_count) return false;
    } else if (pixels * 2 > p->scratch_capacity) return false;
    return
        p->width == p->source_width && p->height == p->source_height &&
        width == p->source_image_width && height == p->source_image_height &&
        width == p->destination_width && height == p->destination_height &&
        p->depth == 1 && p->dimensions == 2 && p->levels == 1 && p->layers == 1 &&
        !p->cubemap && !p->border && p->active && p->initialized &&
        !p->upload_pending && !p->download_pending &&
        p->native_compatible && p->distinct_images;
}
#endif
