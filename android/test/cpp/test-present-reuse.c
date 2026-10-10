/* Check the production redraw policy without a Vulkan device. */
#include <assert.h>
#include <stdio.h>
#include "hw/xbox/nv2a/pgraph/vk/present-reuse.h"

int main(void)
{
    VkPresentReuseCache cache = { 0 };
    VkPresentReuseKey key = {
        .image = 1, .image_view = 2, .display_addr = 3,
        .draw_time = 10, .draw_generation = 11, .surface_list_gen = 12,
        .pitch = 2560, .width = 640, .height = 480, .line_offset = 2560,
        .display_width = 640, .display_height = 480,
        .viewport_x = 40, .viewport_y = 20, .viewport_width = 1440,
        .viewport_height = 1080, .swapchain_width = 2340,
        .swapchain_height = 1080, .swapchain_format = 37,
    };
    assert(!vk_present_can_reuse(&cache, &key, 1000000, false, false, false));
    vk_present_reuse_commit(&cache, &key, 1000000, true);
    assert(vk_present_can_reuse(&cache, &key, 1000000, false, false, false));
    assert(vk_present_can_reuse(&cache, &key, 1249999, false, false, false));
    assert(!vk_present_can_reuse(&cache, &key, 1250000, false, false, false));
    assert(!vk_present_can_reuse(&cache, &key, 999999, false, false, false));

    /* Pending writes, video or thumbnail capture always require a redraw. */
    assert(!vk_present_can_reuse(&cache, &key, 1000001, true, false, false));
    assert(!vk_present_can_reuse(&cache, &key, 1000001, false, true, false));
    assert(!vk_present_can_reuse(&cache, &key, 1000001, false, false, true));

    /* Any changed source, scanout, geometry or output identity must redraw. */
#define CHECK_CHANGE(field) do { \
    VkPresentReuseKey changed = key; \
    changed.field++; \
    assert(!vk_present_can_reuse(&cache, &changed, 1000001, false, false, false)); \
} while (0)
    CHECK_CHANGE(image);
    CHECK_CHANGE(image_view);
    CHECK_CHANGE(display_addr);
    CHECK_CHANGE(draw_time);
    CHECK_CHANGE(draw_generation);
    CHECK_CHANGE(surface_list_gen);
    CHECK_CHANGE(pitch);
    CHECK_CHANGE(width);
    CHECK_CHANGE(height);
    CHECK_CHANGE(line_offset);
    CHECK_CHANGE(display_width);
    CHECK_CHANGE(display_height);
    CHECK_CHANGE(viewport_x);
    CHECK_CHANGE(viewport_y);
    CHECK_CHANGE(viewport_width);
    CHECK_CHANGE(viewport_height);
    CHECK_CHANGE(swapchain_width);
    CHECK_CHANGE(swapchain_height);
    CHECK_CHANGE(swapchain_format);
#undef CHECK_CHANGE

    /* Reuse must not slide the refresh deadline forwards indefinitely. */
    for (int64_t now = 1010000; now < 1250000; now += 10000) {
        assert(vk_present_can_reuse(&cache, &key, now, false, false, false));
        assert(cache.presented_us == 1000000);
    }
    vk_present_reuse_commit(&cache, &key, 1300000, false);
    assert(!vk_present_can_reuse(&cache, &key, 1300001, false, false, false));
    vk_present_reuse_commit(&cache, &key, 1400000, true);
    assert(vk_present_can_reuse(&cache, &key, 1400001, false, false, false));
    cache.valid = false; /* Swapchain destruction/recreation. */
    assert(!vk_present_can_reuse(&cache, &key, 1400001, false, false, false));
    puts("Native presentation reuse policy checks passed");
    return 0;
}
