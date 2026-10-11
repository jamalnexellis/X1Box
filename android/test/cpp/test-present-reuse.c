/* Check the production redraw policy without a Vulkan device. */
#include <assert.h>
#include <stdio.h>
#include "hw/xbox/nv2a/pgraph/vk/present-reuse.h"
#include "hw/xbox/nv2a/pgraph/vk/present-result.h"

int main(void)
{
    VkPresentReuseCache cache = { 0 };
    VkPresentReuseKey key = {
        .image = 1, .image_view = 2, .display_addr = 3,
        .draw_time = 10, .draw_generation = 11, .surface_serial = 12,
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
    CHECK_CHANGE(surface_serial);
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

    /* New offscreen bindings leave the active scanout identity intact. */
    uint64_t serial = 0;
    key.surface_serial = vk_present_next_surface_serial(&serial);
    vk_present_reuse_commit(&cache, &key, 2000000, true);
    for (unsigned int i = 0; i < 1024; i++) {
        assert(vk_present_next_surface_serial(&serial) != key.surface_serial);
        assert(vk_present_can_reuse(&cache, &key, 2000001 + i,
                                   false, false, false));
    }
    /* Same Vulkan handles, content counters and dimensions after recycling
     * or unshelving must redraw on the fresh binding identity alone. */
    VkPresentReuseKey recycled = key;
    recycled.surface_serial = vk_present_next_surface_serial(&serial);
    assert(vk_present_reuse_reasons(&cache, &recycled, 2000001,
                                   false, false, false) ==
           VK_PRESENT_REUSE_BIT(VK_PRESENT_REUSE_SOURCE));
    vk_present_reuse_commit(&cache, &recycled, 2000002, true);
    assert(vk_present_can_reuse(&cache, &recycled, 2000003,
                               false, false, false));
    assert(!vk_present_can_reuse(&cache, &key, 2000003, false, false, false));

    /* Exhaustion never wraps to a previous binding identity. */
    serial = UINT64_MAX - 1;
    key.surface_serial = vk_present_next_surface_serial(&serial);
    assert(key.surface_serial == UINT64_MAX);
    vk_present_reuse_commit(&cache, &key, 3000000, true);
    assert(vk_present_can_reuse(&cache, &key, 3000001, false, false, false));
    for (unsigned int i = 0; i < 2; i++) {
        key.surface_serial = vk_present_next_surface_serial(&serial);
        assert(key.surface_serial == 0 && serial == UINT64_MAX);
        assert(!vk_present_can_reuse(&cache, &key, 3000001,
                                    false, false, false));
        vk_present_reuse_commit(&cache, &key, 3000001, true);
        assert(!cache.valid);
    }

    /* Clock guard has no signed overflow, including extreme jumps. */
    key.surface_serial = 1;
    vk_present_reuse_commit(&cache, &key, INT64_MIN, true);
    assert(vk_present_can_reuse(&cache, &key, INT64_MIN + 249999,
                               false, false, false));
    assert(!vk_present_can_reuse(&cache, &key, INT64_MIN + 250000,
                                false, false, false));
    assert(!vk_present_can_reuse(&cache, &key, INT64_MAX,
                                false, false, false));
    vk_present_reuse_commit(&cache, &key, INT64_MAX, true);
    assert(!vk_present_can_reuse(&cache, &key, INT64_MIN,
                                false, false, false));

    /* Diagnostics report every blocking guard, with content/geometry changes
     * separate from source identity and overlapping upload/video/capture. */
    vk_present_reuse_commit(&cache, &key, 4000000, true);
    VkPresentReuseKey changed = key;
    changed.draw_generation++;
    changed.viewport_width++;
    uint32_t reasons = vk_present_reuse_reasons(&cache, &changed, 4000001,
                                               true, true, true);
    assert(reasons == (VK_PRESENT_REUSE_BIT(VK_PRESENT_REUSE_UPLOAD) |
                       VK_PRESENT_REUSE_BIT(VK_PRESENT_REUSE_VIDEO) |
                       VK_PRESENT_REUSE_BIT(VK_PRESENT_REUSE_CAPTURE) |
                       VK_PRESENT_REUSE_BIT(VK_PRESENT_REUSE_CONTENT) |
                       VK_PRESENT_REUSE_BIT(VK_PRESENT_REUSE_GEOMETRY)));
    /* Both WSI success codes commit a presented image. Other positive statuses
     * and every failure must discard the cached image, including out of date. */
    const VkResult results[] = {
        VK_SUCCESS, VK_SUBOPTIMAL_KHR, VK_NOT_READY, VK_TIMEOUT, VK_EVENT_SET,
        VK_EVENT_RESET, VK_INCOMPLETE, VK_ERROR_OUT_OF_HOST_MEMORY,
        VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_ERROR_DEVICE_LOST,
        VK_ERROR_SURFACE_LOST_KHR, VK_ERROR_OUT_OF_DATE_KHR,
        VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT,
    };
    for (unsigned int i = 0; i < sizeof(results) / sizeof(results[0]); i++) {
        bool accepted = i < 2;
        assert(vk_present_result_is_presented(results[i]) == accepted);
        vk_present_reuse_commit(&cache, &key, 5000000, true);
        vk_present_reuse_commit(&cache, &key, 5000001,
                                vk_present_result_is_presented(results[i]));
        assert(vk_present_can_reuse(&cache, &key, 5000002,
                                   false, false, false) == accepted);
    }
    puts("Native presentation reuse policy checks passed");
    return 0;
}
