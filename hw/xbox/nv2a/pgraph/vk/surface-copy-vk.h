/* Same-queue refresh with stable images/views and explicit prior-reader
 * dependencies. Record outside render passes, after prior queued draws. */
#ifndef XEMU_VK_SURFACE_COPY_VK_H
#define XEMU_VK_SURFACE_COPY_VK_H
#include <stdbool.h>
#include <vulkan/vulkan.h>

static inline bool vk_surface_copy_formats_compatible(VkFormat source,
                                                       VkFormat destination)
{
    /* Preserve the existing vkCmdCopyImage byte interpretation. RGBA/BGRA
     * UNORM copies are size-compatible, not a color conversion or swizzle. */
    if (source == VK_FORMAT_R5G6B5_UNORM_PACK16 || source == VK_FORMAT_R8_UNORM) {
        return destination == source;
    }
    return (source == VK_FORMAT_R8G8B8A8_UNORM ||
            source == VK_FORMAT_B8G8R8A8_UNORM) &&
           (destination == VK_FORMAT_R8G8B8A8_UNORM ||
            destination == VK_FORMAT_B8G8R8A8_UNORM);
}

static inline void vk_surface_copy_record(VkCommandBuffer cmd,
    VkImage source, VkImageLayout source_layout, VkImage destination,
    VkImageLayout destination_layout, uint32_t width, uint32_t height)
{
    VkImageMemoryBarrier barriers[2];
    for (unsigned i = 0; i < 2; i++) {
        barriers[i] = (VkImageMemoryBarrier) {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = i ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = i ? destination_layout : source_layout,
            .newLayout = i ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = i ? destination : source,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
    }
    if (destination_layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        barriers[1].srcAccessMask = 0;
    }
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, barriers);
    VkImageCopy region = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .extent = { width, height, 1 },
    };
    vkCmdCopyImage(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[0].newLayout = source_layout;
    barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 2, barriers);
}
#endif
