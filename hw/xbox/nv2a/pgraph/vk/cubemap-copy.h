/* Strict policy and refresh identity for copying six active RGB565 surfaces. */
#ifndef XEMU_VK_CUBEMAP_COPY_H
#define XEMU_VK_CUBEMAP_COPY_H
#include <stdbool.h>
#include <stdint.h>

typedef struct VkCubeCopyFace {
    uint64_t address, size, image;
    uint32_t width, height, pitch, draw_time, draw_generation;
    bool active, color, swizzle, initialized, native_rgb565;
    bool upload_pending, download_pending;
} VkCubeCopyFace;

typedef struct VkCubeCopyKey {
    uint64_t image[6];
    uint32_t draw_time[6], draw_generation[6], surface_list_gen;
} VkCubeCopyKey;

typedef struct VkCubeCopyCache {
    VkCubeCopyKey key;
    bool valid;
} VkCubeCopyCache;

static inline bool vk_cube_copy_eligible(uint64_t base, uint64_t length,
    uint64_t layer_size, uint32_t width, uint32_t height, uint32_t depth,
    uint32_t dimensionality, uint32_t levels, uint32_t scale,
    bool cubemap, bool border, bool native_rgb565,
    const VkCubeCopyFace faces[6])
{
    if (!cubemap || border || !native_rgb565 || scale != 1 || levels != 1 ||
        depth != 1 || dimensionality != 2 || !width || width != height ||
        width > 4096 || (width & (width - 1))) {
        return false;
    }
    uint64_t bytes = (uint64_t)width * height * 2;
    if (layer_size < bytes || layer_size > UINT64_MAX / 6 ||
        length != layer_size * 6 || base > UINT64_MAX - length) {
        return false;
    }
    for (unsigned i = 0; i < 6; i++) {
        const VkCubeCopyFace *f = &faces[i];
        if (!f->active || !f->color || !f->swizzle || !f->initialized ||
            !f->native_rgb565 || f->upload_pending || f->download_pending ||
            !f->image || f->address != base + layer_size * i ||
            f->width != width || f->height != height ||
            f->pitch != width * 2 || f->size != bytes) {
            return false;
        }
    }
    return true;
}

static inline bool vk_cube_copy_keys_equal(const VkCubeCopyKey *a,
                                           const VkCubeCopyKey *b)
{
    if (a->surface_list_gen != b->surface_list_gen) {
        return false;
    }
    for (unsigned i = 0; i < 6; i++) {
        if (a->image[i] != b->image[i] ||
            a->draw_time[i] != b->draw_time[i] ||
            a->draw_generation[i] != b->draw_generation[i]) {
            return false;
        }
    }
    return true;
}

static inline bool vk_cube_copy_needs_refresh(const VkCubeCopyCache *cache,
                                              const VkCubeCopyKey *key)
{
    return !cache->valid || !vk_cube_copy_keys_equal(&cache->key, key);
}
#endif
