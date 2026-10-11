/* Same-queue depth copy with ordered scratch reuse and prior destination reads.
 * Call outside render passes after flushing deferred draws. */
#ifndef XEMU_VK_DEPTH_COPY_VK_H
#define XEMU_VK_DEPTH_COPY_VK_H
#include <vulkan/vulkan.h>
static inline void vk_depth_copy_record(VkCommandBuffer cmd, VkImage source,
    VkImageLayout source_layout, VkImage destination,
    VkImageLayout destination_layout, VkBuffer scratch,
    uint32_t width, uint32_t height)
{
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
            .subresourceRange = {i ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1},
        };
    }
    if (destination_layout == VK_IMAGE_LAYOUT_UNDEFINED) images[1].srcAccessMask = 0;
    VkBufferMemoryBarrier buffer = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = scratch, .size = (VkDeviceSize)width * height * 2,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1, &buffer, 2, images);
    VkBufferImageCopy region = {
        .imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 },
        .imageExtent = { width, height, 1 },
    };
    vkCmdCopyImageToBuffer(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           scratch, 1, &region);
    buffer.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    buffer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1, &buffer, 0, NULL);
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vkCmdCopyBufferToImage(cmd, scratch, destination,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    images[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    images[0].newLayout = source_layout;
    images[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    images[0].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    images[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    images[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    images[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    images[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 2, images);
}
#endif
