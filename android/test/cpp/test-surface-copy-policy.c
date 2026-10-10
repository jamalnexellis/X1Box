#include <assert.h>
#include <stdio.h>
#include "hw/xbox/nv2a/pgraph/vk/surface-copy.h"

int main(void)
{
    VkSurfaceCopyPolicy valid = {
        .width = 640, .height = 480, .depth = 1, .dimensions = 2,
        .levels = 1, .layers = 1, .scale = 1,
        .source_width = 640, .source_height = 480,
        .color = true, .active = true, .initialized = true,
        .byte_copy_compatible = true, .distinct_images = true,
    };
    assert(vk_surface_copy_eligible(&valid));
    VkSurfaceCopyPolicy wide = valid;
    wide.width = wide.source_width = 1280;
    assert(vk_surface_copy_eligible(&wide));
#define REJECT(field, value) do { VkSurfaceCopyPolicy p = valid; \
    p.field = value; assert(!vk_surface_copy_eligible(&p)); } while (0)
    REJECT(width, 0); REJECT(height, 0); REJECT(width, 4097);
    REJECT(height, 4097); REJECT(source_width, 320); REJECT(source_height, 240);
    REJECT(depth, 2); REJECT(dimensions, 3); REJECT(levels, 2);
    REJECT(layers, 6); REJECT(scale, 2); REJECT(cubemap, true);
    REJECT(border, true); REJECT(color, false); REJECT(active, false);
    REJECT(initialized, false); REJECT(upload_pending, true);
    REJECT(download_pending, true); REJECT(byte_copy_compatible, false);
    REJECT(distinct_images, false);
#undef REJECT
    puts("PASS surface queued-copy eligibility and fallbacks");
    return 0;
}
