/* Opt-in host compilation policy; guest drawing and shader code stay intact. */
#ifndef NV2A_VK_PIPELINE_COMPILE_H
#define NV2A_VK_PIPELINE_COMPILE_H

#include <stdbool.h>
#include <vulkan/vulkan.h>

static inline VkPipelineCreateFlags nv2a_vk_draw_pipeline_flags(bool unoptimized)
{
    return unoptimized ? VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT : 0;
}

#endif
