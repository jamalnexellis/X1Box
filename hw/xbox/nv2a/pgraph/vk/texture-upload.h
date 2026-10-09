/* SPDX-License-Identifier: LGPL-2.0-or-later */
#ifndef NV2A_VK_TEXTURE_UPLOAD_H
#define NV2A_VK_TEXTURE_UPLOAD_H

/* Shared with the on-device pixel comparison test. The source is already
 * decoded RGBA8: unpacking and storing preserves all four byte channels. */
static const char texture_upload_glsl[] =
    "#version 450\n"
    "layout(local_size_x=8, local_size_y=8, local_size_z=1) in;\n"
    "layout(std430, set=0, binding=0) readonly buffer Src { uint pixels[]; };\n"
    "layout(rgba8, set=0, binding=1) uniform writeonly image2D dst;\n"
    "layout(push_constant) uniform Params { uint offset_words, width, height; };\n"
    "void main() {\n"
    "    uvec2 p = gl_GlobalInvocationID.xy;\n"
    "    if (p.x >= width || p.y >= height) return;\n"
    "    imageStore(dst, ivec2(p),\n"
    "        unpackUnorm4x8(pixels[offset_words + p.y * width + p.x]));\n"
    "}\n";

static inline void texture_upload_image_barrier(
    VkCommandBuffer cmd, VkImage image, uint32_t levels,
    VkImageLayout old_layout, VkImageLayout new_layout,
    VkAccessFlags src_access, VkAccessFlags dst_access,
    VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
{
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1 },
    };
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0,
                         0, NULL, 0, NULL, 1, &barrier);
}

static inline void texture_upload_dispatch(
    VkCommandBuffer cmd, VkPipelineLayout layout, VkDescriptorSet set,
    VkDeviceSize offset, uint32_t width, uint32_t height)
{
    assert(offset % 4 == 0);
    uint32_t params[] = { (uint32_t)(offset / 4), width, height };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout,
                            0, 1, &set, 0, NULL);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(params), params);
    vkCmdDispatch(cmd, (width + 7) / 8, (height + 7) / 8, 1);
}

static inline VkComponentSwizzle texture_upload_bgra_component(
    VkComponentSwizzle component, VkComponentSwizzle identity)
{
    if (component == VK_COMPONENT_SWIZZLE_IDENTITY) component = identity;
    if (component == VK_COMPONENT_SWIZZLE_R) return VK_COMPONENT_SWIZZLE_B;
    if (component == VK_COMPONENT_SWIZZLE_B) return VK_COMPONENT_SWIZZLE_R;
    return component;
}

/* BGRA bytes can use the same RGBA storage kernel without a CPU conversion.
 * Compose the original sampling map with the red/blue exchange. Constants,
 * including the forced alpha of XRGB textures, retain their meaning. Storage
 * descriptors use a separate identity view. Raw image copies also retain the
 * same four bytes when a cached texture switches to a render-surface source. */
static inline VkComponentMapping texture_upload_bgra_mapping(VkComponentMapping map)
{
    return (VkComponentMapping) {
        texture_upload_bgra_component(map.r, VK_COMPONENT_SWIZZLE_R),
        texture_upload_bgra_component(map.g, VK_COMPONENT_SWIZZLE_G),
        texture_upload_bgra_component(map.b, VK_COMPONENT_SWIZZLE_B),
        texture_upload_bgra_component(map.a, VK_COMPONENT_SWIZZLE_A),
    };
}

#endif
