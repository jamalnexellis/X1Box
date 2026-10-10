/* Policy for retaining an already presented, unchanged native Vulkan image. */
#ifndef XEMU_VK_PRESENT_REUSE_H
#define XEMU_VK_PRESENT_REUSE_H
#include <stdbool.h>
#include <stdint.h>

typedef struct VkPresentReuseKey {
    uint64_t image, image_view, display_addr;
    uint32_t draw_time, draw_generation, surface_list_gen;
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

static inline bool vk_present_keys_equal(const VkPresentReuseKey *a,
                                         const VkPresentReuseKey *b)
{
    return a->image == b->image && a->image_view == b->image_view &&
        a->display_addr == b->display_addr && a->draw_time == b->draw_time &&
        a->draw_generation == b->draw_generation &&
        a->surface_list_gen == b->surface_list_gen &&
        a->pitch == b->pitch && a->width == b->width && a->height == b->height &&
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

static inline bool vk_present_can_reuse(const VkPresentReuseCache *cache,
                                       const VkPresentReuseKey *key,
                                       int64_t now_us, bool upload_pending,
                                       bool video_active, bool capture_pending)
{
    /* Periodically acquire/present even an idle image so swapchain changes
     * and presentation errors are discovered within a bounded interval. */
    return cache->valid && !upload_pending && !video_active && !capture_pending &&
        now_us >= cache->presented_us &&
        now_us - cache->presented_us < 250000 &&
        vk_present_keys_equal(&cache->key, key);
}

static inline void vk_present_reuse_commit(VkPresentReuseCache *cache,
                                          const VkPresentReuseKey *key,
                                          int64_t now_us, bool successful)
{
    cache->valid = successful;
    if (successful) {
        cache->key = *key;
        cache->presented_us = now_us;
    }
}
#endif
