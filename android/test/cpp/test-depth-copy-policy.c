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
        .native_compatible = true, .distinct_images = true,
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
    REJECT(download_pending,true); REJECT(native_compatible,false);
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
    valid.stencil = true;
    valid.scratch_capacity = 64 * 1024 * 1024;
    valid.output_capacity = 64 * 1024 * 1024;
    valid.stencil_alignment = 256;
    valid.max_workgroup_x = 1024;
    valid.max_invocations = 256;
    valid.max_group_count = 65535;
    assert(vk_depth_copy_eligible(&valid));
    assert(vk_compute_workgroup_size(1024 * 1024, 1024, 256) == 256);
    assert(vk_compute_workgroup_size(513, 1024, 1024) == 1);
    assert(vk_compute_workgroup_size(0, 1024, 1024) == 0);
#define REJECT(field,value) do {VkDepthCopyPolicy p=valid; p.field=value; \
    assert(!vk_depth_copy_eligible(&p));} while(0)
    REJECT(stencil_alignment,0); REJECT(stencil_alignment,3);
    REJECT(stencil_alignment,UINT64_MAX); REJECT(max_workgroup_x,0);
    REJECT(max_invocations,0); REJECT(max_group_count,4095);
    REJECT(scratch_capacity,5242879); REJECT(output_capacity,4194303);
#undef REJECT
    VkDepthCopyPolicy p=valid; p.scale=3; p.allow_scaled=true;
    p.source_image_width=p.source_image_height=3072;
    p.destination_width=p.destination_height=3072;
    assert(vk_depth_copy_eligible(&p));
    p.scale=4;
    p.source_image_width=p.source_image_height=4096;
    p.destination_width=p.destination_height=4096;
    assert(!vk_depth_copy_eligible(&p)); /* combined scratch exceeds 64 MiB */
    p=valid; p.width=p.source_width=p.source_image_width=p.destination_width=17;
    p.height=p.source_height=p.source_image_height=p.destination_height=17;
    assert(!vk_depth_copy_eligible(&p)); /* partial stencil words */
    puts("PASS D16/D24 queued-copy eligibility, extents, scratch bounds and dispatch limits");
    return 0;
}
