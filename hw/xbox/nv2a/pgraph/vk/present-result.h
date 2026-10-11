/* Successful presentation includes a usable, suboptimal swapchain. */
#ifndef XEMU_VK_PRESENT_RESULT_H
#define XEMU_VK_PRESENT_RESULT_H
#include <stdbool.h>
#include <vulkan/vulkan_core.h>

static inline bool vk_present_result_is_presented(VkResult result)
{
    return result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR;
}
#endif
