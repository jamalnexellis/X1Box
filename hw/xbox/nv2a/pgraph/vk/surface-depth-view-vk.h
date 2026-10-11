/* The surface owns this sampled view until its image can be safely retired.
 * Moving a binding transfers ownership; destroying a recorded view is invalid. */
#ifndef XEMU_VK_SURFACE_DEPTH_VIEW_VK_H
#define XEMU_VK_SURFACE_DEPTH_VIEW_VK_H
#include <assert.h>
#include <vulkan/vulkan.h>

static inline VkResult vk_surface_depth_view_get(VkDevice device, VkImage image,
    VkFormat format, VkImageView *view)
{
    if (*view != VK_NULL_HANDLE) {
        return VK_SUCCESS;
    }
    VkImageViewCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format,
        .subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 },
    };
    return vkCreateImageView(device, &info, NULL, view);
}

static inline void vk_surface_depth_view_move(VkImageView *destination,
                                              VkImageView *source)
{
    assert(destination != source && *destination == VK_NULL_HANDLE);
    *destination = *source;
    *source = VK_NULL_HANDLE;
}

/* The caller must have completed all commands referencing the owner image. */
static inline void vk_surface_depth_view_release(VkDevice device, VkImageView *view)
{
    vkDestroyImageView(device, *view, NULL);
    *view = VK_NULL_HANDLE;
}
#endif
