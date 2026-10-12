/* Host-visible draw data belongs to a fenced submission slot. */
#ifndef NV2A_VK_DRAW_UPLOAD_H
#define NV2A_VK_DRAW_UPLOAD_H
#include <assert.h>
#include <stdbool.h>
#include <vulkan/vulkan.h>

typedef enum NV2AVkDrawUpload {
    NV2A_VK_UPLOAD_INDEX,
    NV2A_VK_UPLOAD_VERTEX,
    NV2A_VK_UPLOAD_UNIFORM,
} NV2AVkDrawUpload;

static inline VkBufferUsageFlags nv2a_vk_draw_upload_usage(NV2AVkDrawUpload type)
{
    switch (type) {
    case NV2A_VK_UPLOAD_INDEX:
        return VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    case NV2A_VK_UPLOAD_VERTEX:
        return VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    case NV2A_VK_UPLOAD_UNIFORM:
        return VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    default:
        assert(false); return 0;
    }
}

static inline void nv2a_vk_draw_upload_barrier(VkCommandBuffer cmd,
    VkBuffer buffer, VkDeviceSize size, NV2AVkDrawUpload type)
{
    VkPipelineStageFlags stages;
    VkAccessFlags access;
    switch (type) {
    case NV2A_VK_UPLOAD_INDEX:
        stages = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
        access = VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
        break;
    case NV2A_VK_UPLOAD_VERTEX:
        stages = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
        access = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
        break;
    case NV2A_VK_UPLOAD_UNIFORM:
        stages = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        access = VK_ACCESS_UNIFORM_READ_BIT;
        break;
    default:
        assert(false); return;
    }
    VkBufferMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT, .dstAccessMask = access,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffer, .size = size,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, stages, 0,
                         0, NULL, 1, &barrier, 0, NULL);
}
#endif
