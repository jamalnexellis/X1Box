/* Policy for retaining an already presented, unchanged native Vulkan image. */
#ifndef XEMU_VK_PRESENT_REUSE_H
#define XEMU_VK_PRESENT_REUSE_H
#include <stdbool.h>
#include <stdint.h>

typedef struct VkPresentReuseKey {
    uint64_t image, image_view, display_addr, surface_serial;
    uint32_t draw_time, draw_generation;
    uint32_t pitch, width, height, line_offset;
    uint32_t display_width, display_height;
    int32_t viewport_x, viewport_y;
    uint32_t viewport_width, viewport_height;
    uint32_t swapchain_width, swapchain_height, swapchain_format;
} VkPresentReuseKey;

typedef struct VkPresentReuseCache {
    VkPresentReuseKey key;
    int64_t presented_us;
    bool valid;
} VkPresentReuseCache;

/* Assign a new identity on every active binding lifetime, including recycled
 * images and unshelved surfaces. Zero means reuse is disabled. Saturate rather
 * than wrap, so recycled Vulkan handles can never alias an earlier identity. */
static inline uint64_t vk_present_next_surface_serial(uint64_t *last_serial)
{
    if (*last_serial == UINT64_MAX) {
        return 0;
    }
    return ++*last_serial;
}

static inline bool vk_present_same_source(const VkPresentReuseKey *a,
                                         const VkPresentReuseKey *b)
{
    return a->image == b->image && a->image_view == b->image_view &&
        a->display_addr == b->display_addr &&
        a->surface_serial == b->surface_serial;
}

static inline bool vk_present_same_content(const VkPresentReuseKey *a,
                                          const VkPresentReuseKey *b)
{
    return a->draw_time == b->draw_time &&
        a->draw_generation == b->draw_generation;
}

static inline bool vk_present_same_geometry(const VkPresentReuseKey *a,
                                           const VkPresentReuseKey *b)
{
    return a->pitch == b->pitch && a->width == b->width && a->height == b->height &&
        a->line_offset == b->line_offset &&
        a->display_width == b->display_width &&
        a->display_height == b->display_height &&
        a->viewport_x == b->viewport_x && a->viewport_y == b->viewport_y &&
        a->viewport_width == b->viewport_width &&
        a->viewport_height == b->viewport_height &&
        a->swapchain_width == b->swapchain_width &&
        a->swapchain_height == b->swapchain_height &&
        a->swapchain_format == b->swapchain_format;
}

static inline bool vk_present_keys_equal(const VkPresentReuseKey *a,
                                         const VkPresentReuseKey *b)
{
    return vk_present_same_source(a, b) && vk_present_same_content(a, b) &&
        vk_present_same_geometry(a, b);
}

enum VkPresentReuseReason {
    VK_PRESENT_REUSE_COLD,
    VK_PRESENT_REUSE_INVALID_SERIAL,
    VK_PRESENT_REUSE_UPLOAD,
    VK_PRESENT_REUSE_VIDEO,
    VK_PRESENT_REUSE_CAPTURE,
    VK_PRESENT_REUSE_CLOCK,
    VK_PRESENT_REUSE_SOURCE,
    VK_PRESENT_REUSE_CONTENT,
    VK_PRESENT_REUSE_GEOMETRY,
    VK_PRESENT_REUSE_REASON_COUNT,
};

#define VK_PRESENT_REUSE_BIT(reason) (1u << (reason))

/* Reasons can overlap. This is also the production decision, so diagnostics
 * describe the guards actually applied rather than a separate approximation. */
static inline uint32_t vk_present_reuse_reasons(const VkPresentReuseCache *cache,
                                              const VkPresentReuseKey *key,
                                              int64_t now_us,
                                              bool upload_pending,
                                              bool video_active,
                                              bool capture_pending)
{
    uint32_t reasons = 0;
    if (!cache->valid) {
        reasons |= VK_PRESENT_REUSE_BIT(VK_PRESENT_REUSE_COLD);
    }
    if (!key->surface_serial) {
        reasons |= VK_PRESENT_REUSE_BIT(VK_PRESENT_REUSE_INVALID_SERIAL);
    }
    if (upload_pending) {
        reasons |= VK_PRESENT_REUSE_BIT(VK_PRESENT_REUSE_UPLOAD);
    }
    if (video_active) {
        reasons |= VK_PRESENT_REUSE_BIT(VK_PRESENT_REUSE_VIDEO);
    }
    if (capture_pending) {
        reasons |= VK_PRESENT_REUSE_BIT(VK_PRESENT_REUSE_CAPTURE);
    }
    /* Periodically acquire/present even an idle image so swapchain changes
     * and presentation errors are discovered within a bounded interval. */
    if (cache->valid) {
        /* Unsigned subtraction avoids signed overflow at clock extremes. */
        if (now_us < cache->presented_us ||
            (uint64_t)now_us - (uint64_t)cache->presented_us >= 250000) {
            reasons |= VK_PRESENT_REUSE_BIT(VK_PRESENT_REUSE_CLOCK);
        }
        if (!vk_present_same_source(&cache->key, key)) {
            reasons |= VK_PRESENT_REUSE_BIT(VK_PRESENT_REUSE_SOURCE);
        }
        if (!vk_present_same_content(&cache->key, key)) {
            reasons |= VK_PRESENT_REUSE_BIT(VK_PRESENT_REUSE_CONTENT);
        }
        if (!vk_present_same_geometry(&cache->key, key)) {
            reasons |= VK_PRESENT_REUSE_BIT(VK_PRESENT_REUSE_GEOMETRY);
        }
    }
    return reasons;
}

static inline bool vk_present_can_reuse(const VkPresentReuseCache *cache,
                                       const VkPresentReuseKey *key,
                                       int64_t now_us, bool upload_pending,
                                       bool video_active, bool capture_pending)
{
    return vk_present_reuse_reasons(cache, key, now_us, upload_pending,
                                   video_active, capture_pending) == 0;
}

static inline void vk_present_reuse_commit(VkPresentReuseCache *cache,
                                          const VkPresentReuseKey *key,
                                          int64_t now_us, bool successful)
{
    cache->valid = successful && key->surface_serial != 0;
    if (cache->valid) {
        cache->key = *key;
        cache->presented_us = now_us;
    }
}
#endif
