/* Owned vertex layouts for immediate and deferred dynamic-input draws. */
#ifndef NV2A_VK_VERTEX_INPUT_H
#define NV2A_VK_VERTEX_INPUT_H

#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define NV2A_VK_VERTEX_INPUT_MAX 16

typedef struct NV2AVkVertexInput {
    uint32_t binding_count;
    uint32_t attribute_count;
    VkVertexInputBindingDescription bindings[NV2A_VK_VERTEX_INPUT_MAX];
    VkVertexInputAttributeDescription attributes[NV2A_VK_VERTEX_INPUT_MAX];
} NV2AVkVertexInput;

static inline void vk_vertex_input_snapshot(
    NV2AVkVertexInput *out, uint32_t binding_count,
    const VkVertexInputBindingDescription *bindings, uint32_t attribute_count,
    const VkVertexInputAttributeDescription *attributes)
{
    assert(binding_count <= NV2A_VK_VERTEX_INPUT_MAX);
    assert(attribute_count <= NV2A_VK_VERTEX_INPUT_MAX);
    memset(out, 0, sizeof(*out));
    out->binding_count = binding_count;
    out->attribute_count = attribute_count;
    if (binding_count) {
        memcpy(out->bindings, bindings, binding_count * sizeof(*bindings));
    }
    if (attribute_count) {
        memcpy(out->attributes, attributes,
               attribute_count * sizeof(*attributes));
    }
}

static inline void vk_vertex_input_emit(PFN_vkCmdSetVertexInputEXT set_input,
                                       VkCommandBuffer cmd,
                                       const NV2AVkVertexInput *input)
{
    assert(set_input);
    assert(input->binding_count <= NV2A_VK_VERTEX_INPUT_MAX);
    assert(input->attribute_count <= NV2A_VK_VERTEX_INPUT_MAX);
    VkVertexInputBindingDescription2EXT bindings[NV2A_VK_VERTEX_INPUT_MAX];
    VkVertexInputAttributeDescription2EXT attributes[NV2A_VK_VERTEX_INPUT_MAX];
    for (uint32_t i = 0; i < input->binding_count; i++) {
        const VkVertexInputBindingDescription *b = &input->bindings[i];
        bindings[i] = (VkVertexInputBindingDescription2EXT){
            .sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT,
            .binding = b->binding,
            .stride = b->stride,
            .inputRate = b->inputRate,
            .divisor = 1,
        };
    }
    for (uint32_t i = 0; i < input->attribute_count; i++) {
        const VkVertexInputAttributeDescription *a = &input->attributes[i];
        attributes[i] = (VkVertexInputAttributeDescription2EXT){
            .sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT,
            .location = a->location,
            .binding = a->binding,
            .format = a->format,
            .offset = a->offset,
        };
    }
    /* Zero-input draws still define the dynamic state in this command buffer. */
    set_input(cmd, input->binding_count,
              input->binding_count ? bindings : NULL,
              input->attribute_count,
              input->attribute_count ? attributes : NULL);
}

#endif
