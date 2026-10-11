/* D24/S8 -> RGBA bytes on one queue. Dispatch packs raw depth/stencil bits.
 * The caller owns descriptors until submission completion and checks extents,
 * scratch capacities/alignment and dispatch limits before recording. */
#ifndef XEMU_VK_DEPTH_STENCIL_COPY_VK_H
#define XEMU_VK_DEPTH_STENCIL_COPY_VK_H
#include <vulkan/vulkan.h>
typedef void (*VkDepthStencilPack)(void *, VkCommandBuffer, VkBuffer, VkBuffer);

static inline void vk_depth_stencil_copy_record(VkCommandBuffer cmd,
    VkImage source, VkImageLayout source_layout, VkImage destination,
    VkImageLayout destination_layout, VkBuffer input, VkBuffer output,
    VkDeviceSize stencil_offset, uint32_t width, uint32_t height,
    VkDepthStencilPack pack, void *user)
{
    VkDeviceSize pixels = (VkDeviceSize)width * height;
    VkImageMemoryBarrier images[2];
    for (unsigned i = 0; i < 2; i++) {
        images[i] = (VkImageMemoryBarrier) {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = i ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = i ? destination_layout : source_layout,
            .newLayout = i ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = i ? destination : source,
            .subresourceRange = {i ? VK_IMAGE_ASPECT_COLOR_BIT :
                VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1},
        };
    }
    if (destination_layout == VK_IMAGE_LAYOUT_UNDEFINED) images[1].srcAccessMask = 0;
    VkBufferMemoryBarrier buffers[2];
    for (unsigned i = 0; i < 2; i++) {
        buffers[i] = (VkBufferMemoryBarrier) {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = i ? VK_ACCESS_SHADER_WRITE_BIT : VK_ACCESS_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = i ? output : input,
            .size = i ? pixels * 4 : stencil_offset + pixels,
        };
    }
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, NULL, 2, buffers, 2, images);
    VkBufferImageCopy regions[2] = {
        {.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
         .imageExtent = {width, height, 1}},
        {.bufferOffset = stencil_offset,
         .imageSubresource = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1},
         .imageExtent = {width, height, 1}},
    };
    vkCmdCopyImageToBuffer(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, input, 2, regions);
    buffers[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    buffers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 1, buffers, 0, NULL);
    pack(user, cmd, input, output);
    buffers[1].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    buffers[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1, &buffers[1], 0, NULL);
    regions[0].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vkCmdCopyBufferToImage(cmd, output, destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, regions);
    images[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    images[0].newLayout = source_layout;
    images[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    images[0].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    images[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    images[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    images[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    images[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    buffers[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    buffers[1].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    for (unsigned i = 0; i < 2; i++) buffers[i].dstAccessMask =
        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 2, buffers, 2, images);
}
#endif
