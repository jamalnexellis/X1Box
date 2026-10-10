/* Record the copy on the graphics queue, preserving source layouts and prior
 * destination readers. Shared with the standalone Vulkan pixel fixture. */
#ifndef XEMU_VK_CUBEMAP_COPY_VK_H
#define XEMU_VK_CUBEMAP_COPY_VK_H
#include <vulkan/vulkan.h>

static inline void vk_cube_copy_record(VkCommandBuffer cmd,
    const VkImage sources[6], const VkImageLayout layouts[6], VkImage destination,
    VkImageLayout destination_layout, uint32_t width, uint32_t height)
{
    VkImageMemoryBarrier barriers[7] = { 0 };
    for (unsigned i = 0; i < 7; i++) {
        barriers[i] = (VkImageMemoryBarrier) {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = i < 6 ? VK_ACCESS_TRANSFER_READ_BIT :
                                    VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = i < 6 ? layouts[i] : destination_layout,
            .newLayout = i < 6 ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL :
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = i < 6 ? sources[i] : destination,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
                                  i < 6 ? 1 : 6 },
        };
    }
    if (destination_layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        barriers[6].srcAccessMask = 0;
    }
    /* ALL_COMMANDS covers prior render, shader reads and transfer operations
     * in this queue, including earlier submitted frames. No host wait or
     * descriptor/image replacement is needed to update image contents. */
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 7, barriers);
    for (unsigned i = 0; i < 6; i++) {
        VkImageCopy region = {
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, i, 1 },
            .extent = { width, height, 1 },
        };
        vkCmdCopyImage(cmd, sources[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        barriers[i].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[i].newLayout = layouts[i];
        barriers[i].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barriers[i].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT |
                                    VK_ACCESS_MEMORY_WRITE_BIT;
    }
    barriers[6].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[6].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[6].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[6].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 7, barriers);
}
#endif
