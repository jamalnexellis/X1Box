#include <assert.h>
#include <stdio.h>
#include "hw/xbox/nv2a/pgraph/vk/depth-copy.h"
int main(void)
{
    VkDepthCopyPolicy valid = {
        .width = 1024, .height = 1024, .scale = 1, .depth = 1,
        .dimensions = 2, .levels = 1, .layers = 1,
        .source_width = 1024, .source_height = 1024,
        .source_image_width = 1024, .source_image_height = 1024,
        .destination_width = 1024, .destination_height = 1024,
        .scratch_capacity = 32 * 1024 * 1024,
        .active = true, .initialized = true,
        .native_d16_r16 = true, .distinct_images = true,
    };
    assert(vk_depth_copy_eligible(&valid));
#define REJECT(field,value) do {VkDepthCopyPolicy p=valid; p.field=value; \
    assert(!vk_depth_copy_eligible(&p));} while(0)
    REJECT(width,0); REJECT(height,0); REJECT(scale,0); REJECT(scale,2);
    REJECT(scale,UINT32_MAX); REJECT(width,UINT32_MAX);
    REJECT(depth,2); REJECT(dimensions,3); REJECT(levels,2); REJECT(layers,6);
    REJECT(source_width,512); REJECT(source_height,512);
    REJECT(source_image_width,512); REJECT(source_image_height,512);
    REJECT(destination_width,512); REJECT(destination_height,512);
    REJECT(scratch_capacity,2097151); REJECT(cubemap,true); REJECT(border,true);
    REJECT(active,false); REJECT(initialized,false); REJECT(upload_pending,true);
    REJECT(download_pending,true); REJECT(native_d16_r16,false);
    REJECT(distinct_images,false);
#undef REJECT
    for(unsigned scale=2;scale<=4;scale++) {
        VkDepthCopyPolicy p=valid; p.scale=scale; p.allow_scaled=true;
        p.source_image_width=p.source_image_height=1024*scale;
        p.destination_width=p.destination_height=1024*scale;
        assert(vk_depth_copy_eligible(&p));
        p.scratch_capacity=(uint64_t)1024*scale*1024*scale*2-1;
        assert(!vk_depth_copy_eligible(&p));
    }
    puts("PASS queued D16/R16 eligibility, scaled extents and scratch capacity");
    return 0;
}
