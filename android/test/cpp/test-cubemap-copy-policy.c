/* Reject incomplete/incompatible cubes and stale identities of any face. */
#include <assert.h>
#include <stdio.h>
#include "hw/xbox/nv2a/pgraph/vk/cubemap-copy.h"

static bool eligible(const VkCubeCopyFace f[6])
{
    return vk_cube_copy_eligible(0x100000, 786432, 131072, 256, 256, 1,
                                2, 1, 1, true, false, true, f);
}

int main(void)
{
    VkCubeCopyFace faces[6];
    VkCubeCopyKey key = { .surface_list_gen = 10 };
    for (unsigned i = 0; i < 6; i++) {
        faces[i] = (VkCubeCopyFace) {
            .address = 0x100000 + 131072 * i, .size = 131072, .image = i + 1,
            .width = 256, .height = 256, .pitch = 512, .active = true,
            .color = true, .swizzle = true, .initialized = true,
            .native_rgb565 = true,
        };
        key.image[i] = i + 1;
        key.draw_time[i] = 12;
        key.draw_generation[i] = 13;
    }
    assert(eligible(faces));
    /* Every face matters: a compatible face zero cannot authorize five
     * missing, shelved, CPU-modified or differently interpreted faces. */
    for (unsigned i = 0; i < 6; i++) {
#define REJECT(field, value) do { \
    VkCubeCopyFace saved = faces[i]; \
    faces[i].field = (value); assert(!eligible(faces)); faces[i] = saved; \
} while (0)
        REJECT(active, false);
        REJECT(color, false);
        REJECT(swizzle, false);
        REJECT(initialized, false);
        REJECT(native_rgb565, false);
        REJECT(upload_pending, true);
        REJECT(download_pending, true);
        REJECT(image, 0);
        REJECT(address, faces[i].address + 2);
        REJECT(size, 131070);
        REJECT(width, 128);
        REJECT(height, 128);
        REJECT(pitch, 1024);
#undef REJECT
    }
#define SHAPE(w,h,d,dim,mips,scale,cube,border,fmt) \
    vk_cube_copy_eligible(0x100000, 786432, 131072, w,h,d,dim,mips,scale, \
                          cube,border,fmt,faces)
    assert(!SHAPE(256,256,1,2,1,2,true,false,true));
    assert(!SHAPE(256,256,1,2,2,1,true,false,true));
    assert(!SHAPE(256,256,1,2,1,1,true,true,true));
    assert(!SHAPE(256,256,1,2,1,1,false,false,true));
    assert(!SHAPE(256,256,1,2,1,1,true,false,false));
    assert(!SHAPE(256,256,2,3,1,1,true,false,true));
    assert(!SHAPE(256,128,1,2,1,1,true,false,true));
    assert(!SHAPE(0,0,1,2,1,1,true,false,true));
    assert(!SHAPE(255,255,1,2,1,1,true,false,true));
#undef SHAPE
    assert(!vk_cube_copy_eligible(UINT64_MAX - 100, 786432, 131072,
        256,256,1,2,1,1,true,false,true,faces));
    assert(!vk_cube_copy_eligible(0x100000, 786431, 131072,
        256,256,1,2,1,1,true,false,true,faces));
    assert(!vk_cube_copy_eligible(0x100000, 786432, UINT64_MAX,
        256,256,1,2,1,1,true,false,true,faces));

    VkCubeCopyCache cache = { 0 };
    assert(vk_cube_copy_needs_refresh(&cache, &key));
    cache.key = key;
    cache.valid = true;
    assert(!vk_cube_copy_needs_refresh(&cache, &key));
    for (unsigned i = 0; i < 6; i++) {
        VkCubeCopyKey changed = key;
        changed.image[i]++;
        assert(vk_cube_copy_needs_refresh(&cache, &changed));
        changed = key; changed.draw_time[i]++;
        assert(vk_cube_copy_needs_refresh(&cache, &changed));
        changed = key; changed.draw_generation[i]++;
        assert(vk_cube_copy_needs_refresh(&cache, &changed));
    }
    VkCubeCopyKey recreated = key;
    recreated.surface_list_gen++; /* Handle reused by a different surface. */
    assert(vk_cube_copy_needs_refresh(&cache, &recreated));
    cache.valid = false; /* CPU write, even if source handles are unchanged. */
    assert(vk_cube_copy_needs_refresh(&cache, &key));
    puts("Six-face cubemap eligibility and refresh policy checks passed");
    return 0;
}
