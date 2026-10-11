#include <assert.h>
#include <stdio.h>
#include "hw/xbox/nv2a/pgraph/vk/surface-copy.h"

int main(void)
{
    VkSurfaceCopyPolicy valid = {
        .width = 640, .height = 480, .depth = 1, .dimensions = 2,
        .levels = 1, .layers = 1, .scale = 1,
        .source_width = 640, .source_height = 480,
        .source_image_width = 640, .source_image_height = 480,
        .destination_width = 640, .destination_height = 480,
        .color = true, .active = true, .initialized = true,
        .byte_copy_compatible = true, .distinct_images = true,
    };
    assert(vk_surface_copy_eligible(&valid));
    VkSurfaceCopyPolicy wide = valid;
    wide.width = wide.source_width = 1280;
    wide.source_image_width = wide.destination_width = 1280;
    assert(vk_surface_copy_eligible(&wide));
    assert(!vk_surface_copy_storage_border(false, false));
    assert(!vk_surface_copy_storage_border(false, true));
    assert(!vk_surface_copy_storage_border(true, true));
    assert(vk_surface_copy_storage_border(true, false));
    wide.border = vk_surface_copy_storage_border(true, true);
    assert(vk_surface_copy_rejections(&wide) == 0);
    wide.border = vk_surface_copy_storage_border(true, false);
    assert(vk_surface_copy_rejections(&wide) == VK_SURFACE_COPY_BORDER);
    wide.upload_pending = true;
    assert(vk_surface_copy_rejections(&wide) ==
           (VK_SURFACE_COPY_BORDER | VK_SURFACE_COPY_PENDING));
#define REJECT(field, value) do { VkSurfaceCopyPolicy p = valid; \
    p.field = value; assert(!vk_surface_copy_eligible(&p)); } while (0)
    REJECT(width, 0); REJECT(height, 0); REJECT(width, 4097);
    REJECT(height, 4097); REJECT(source_width, 320); REJECT(source_height, 240);
    REJECT(depth, 2); REJECT(dimensions, 3); REJECT(levels, 2);
    REJECT(layers, 6); REJECT(scale, 2); REJECT(cubemap, true);
    REJECT(source_image_width, 320); REJECT(source_image_height, 240);
    REJECT(destination_width, 1280); REJECT(destination_height, 960);
    REJECT(border, true); REJECT(color, false); REJECT(active, false);
    REJECT(initialized, false); REJECT(upload_pending, true);
    REJECT(download_pending, true); REJECT(byte_copy_compatible, false);
    REJECT(distinct_images, false);
#undef REJECT
    for (unsigned scale = 2; scale <= 4; scale++) {
        VkSurfaceCopyPolicy scaled = valid;
        scaled.scale = scale;
        scaled.source_image_width = scaled.destination_width = 640 * scale;
        scaled.source_image_height = scaled.destination_height = 480 * scale;
        assert(!vk_surface_copy_eligible(&scaled)); /* opt-in required */
        scaled.allow_scaled = true;
        assert(vk_surface_copy_eligible(&scaled));
        scaled.source_image_width = 640; /* source from the previous scale */
        assert(!vk_surface_copy_eligible(&scaled));
        scaled.source_image_width = 640 * scale;
        scaled.destination_height = 480; /* native CPU fallback allocation */
        assert(!vk_surface_copy_eligible(&scaled));
    }
    uint32_t width = 0, height = 0;
    assert(vk_scaled_copy_extent(1280, 480, 3, true, &width, &height));
    assert(width == 3840 && height == 1440);
    assert(!vk_scaled_copy_extent(1280, 480, 4, true, &width, &height));
    assert(!vk_scaled_copy_extent(UINT32_MAX, 480, 3, true, &width, &height));
    assert(!vk_scaled_copy_extent(640, 480, 0, true, &width, &height));
    assert(!vk_scaled_copy_extent(640, 480, 5, true, &width, &height));
    assert(!vk_scaled_copy_extent(640, 480, UINT32_MAX, true, &width, &height));
    VkSurfaceCopyPolicy rejected = valid;
    rejected.width = 0;
    rejected.color = false;
    rejected.byte_copy_compatible = false;
    rejected.distinct_images = false;
    assert(vk_surface_copy_rejections(&rejected) ==
           (VK_SURFACE_COPY_GEOMETRY | VK_SURFACE_COPY_SOURCE |
            VK_SURFACE_COPY_FORMAT_ALLOCATION | VK_SURFACE_COPY_IMAGES));
    puts("PASS surface queued-copy eligibility and fallbacks");
    return 0;
}
