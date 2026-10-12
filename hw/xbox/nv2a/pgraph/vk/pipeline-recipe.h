/* Owned, pointer-free descriptions used only to warm the driver cache. */
#ifndef NV2A_VK_PIPELINE_RECIPE_H
#define NV2A_VK_PIPELINE_RECIPE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <vulkan/vulkan.h>
#include "vertex-input.h"

#define NV2A_VK_RECIPE_ABI 1u
#define NV2A_VK_RECIPE_MAX 1024u
#define NV2A_VK_RECIPE_SHADER_MAX (4u * 1024u * 1024u)

typedef struct NV2AVkPipelineRecipe {
    uint32_t flags, stage_count, stages[3];
    uint8_t shader_ids[3][32];
    NV2AVkVertexInput input;
    uint32_t dynamic_input, topology;
    uint32_t static_viewport, static_scissor;
    VkViewport viewport;
    VkRect2D scissor;
    uint32_t depth_clamp, discard, polygon, cull, front_face, depth_bias;
    float bias_constant, bias_clamp, bias_slope, line_width;
    uint32_t has_depth, depth_test, depth_write, depth_compare, stencil_test;
    VkStencilOpState front, back;
    uint32_t has_color, logic_op;
    VkPipelineColorBlendAttachmentState blend;
    float blend_constants[4];
    uint32_t dynamic_count;
    VkDynamicState dynamic_states[20];
    uint32_t set_count, push_count;
    VkPushConstantRange push_ranges[2];
    uint32_t color_format, depth_format;
} NV2AVkPipelineRecipe;

typedef struct NV2AVkPipelineReplayInfo {
    VkPipelineShaderStageCreateInfo stages[3];
    VkPipelineVertexInputStateCreateInfo input;
    VkPipelineInputAssemblyStateCreateInfo assembly;
    VkPipelineViewportStateCreateInfo viewport;
    VkPipelineRasterizationStateCreateInfo raster;
    VkPipelineDepthStencilStateCreateInfo depth;
    VkPipelineMultisampleStateCreateInfo samples;
    VkPipelineColorBlendStateCreateInfo blend;
    VkPipelineDynamicStateCreateInfo dynamic;
    VkGraphicsPipelineCreateInfo pipeline;
} NV2AVkPipelineReplayInfo;

static inline bool nv2a_vk_recipe_dynamic_state(VkDynamicState state)
{
    switch (state) {
    case VK_DYNAMIC_STATE_VIEWPORT: case VK_DYNAMIC_STATE_SCISSOR:
    case VK_DYNAMIC_STATE_BLEND_CONSTANTS: case VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK:
    case VK_DYNAMIC_STATE_STENCIL_WRITE_MASK: case VK_DYNAMIC_STATE_STENCIL_REFERENCE:
    case VK_DYNAMIC_STATE_LINE_WIDTH: case VK_DYNAMIC_STATE_DEPTH_BIAS:
    case VK_DYNAMIC_STATE_CULL_MODE: case VK_DYNAMIC_STATE_FRONT_FACE:
    case VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE: case VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE:
    case VK_DYNAMIC_STATE_DEPTH_COMPARE_OP: case VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE:
    case VK_DYNAMIC_STATE_STENCIL_OP: case VK_DYNAMIC_STATE_VERTEX_INPUT_EXT:
    case VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT:
    case VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT:
    case VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT:
        return true;
    default:
        return false;
    }
}

static inline bool nv2a_vk_recipe_capture(
    NV2AVkPipelineRecipe *out, const VkGraphicsPipelineCreateInfo *p,
    const VkPipelineLayoutCreateInfo *layout, VkFormat color, VkFormat depth)
{
    if (!p || !layout || p->stageCount < 2 || p->stageCount > 3 || !p->pStages ||
        !p->pInputAssemblyState || !p->pViewportState || !p->pRasterizationState ||
        !p->pMultisampleState || !p->pColorBlendState || p->subpass ||
        layout->setLayoutCount > 2 || layout->pushConstantRangeCount > 2 ||
        layout->pNext || layout->flags || p->pTessellationState ||
        p->pInputAssemblyState->primitiveRestartEnable ||
        p->pViewportState->viewportCount != 1 || p->pViewportState->scissorCount != 1 ||
        p->pRasterizationState->pNext || p->pRasterizationState->flags ||
        p->pMultisampleState->rasterizationSamples != VK_SAMPLE_COUNT_1_BIT ||
        p->pMultisampleState->sampleShadingEnable || p->pMultisampleState->alphaToCoverageEnable ||
        p->pMultisampleState->alphaToOneEnable || p->pMultisampleState->pSampleMask ||
        p->pColorBlendState->logicOpEnable || p->pColorBlendState->attachmentCount > 1) {
        return false;
    }
    uint32_t bindings = p->pVertexInputState ? p->pVertexInputState->vertexBindingDescriptionCount : 0;
    uint32_t attrs = p->pVertexInputState ? p->pVertexInputState->vertexAttributeDescriptionCount : 0;
    uint32_t dynamic = p->pDynamicState ? p->pDynamicState->dynamicStateCount : 0;
    const VkBaseInStructure *next = p->pNext;
    if ((next && (next->sType != VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO || next->pNext)) ||
        p->pInputAssemblyState->pNext || p->pInputAssemblyState->flags ||
        p->pViewportState->pNext || p->pViewportState->flags ||
        p->pMultisampleState->pNext || p->pMultisampleState->flags ||
        p->pColorBlendState->pNext || p->pColorBlendState->flags ||
        (p->pVertexInputState && (p->pVertexInputState->pNext || p->pVertexInputState->flags)) ||
        (p->pDynamicState && (p->pDynamicState->pNext || p->pDynamicState->flags))) return false;
    if (bindings > 16 || attrs > 16 || dynamic > 20 ||
        (bindings && !p->pVertexInputState->pVertexBindingDescriptions) ||
        (attrs && !p->pVertexInputState->pVertexAttributeDescriptions) ||
        (dynamic && !p->pDynamicState->pDynamicStates) ||
        (layout->pushConstantRangeCount && !layout->pPushConstantRanges) ||
        (p->pColorBlendState->attachmentCount && !p->pColorBlendState->pAttachments)) {
        return false;
    }
    if (p->pDepthStencilState && (p->pDepthStencilState->pNext ||
        p->pDepthStencilState->flags || p->pDepthStencilState->depthBoundsTestEnable)) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->flags = p->flags;
    out->stage_count = p->stageCount;
    for (uint32_t i = 0; i < p->stageCount; i++) {
        if (p->pStages[i].pNext || p->pStages[i].pSpecializationInfo ||
            p->pStages[i].flags || !p->pStages[i].pName || strcmp(p->pStages[i].pName, "main")) {
            return false;
        }
        out->stages[i] = p->pStages[i].stage;
    }
    vk_vertex_input_snapshot(&out->input, bindings,
        bindings ? p->pVertexInputState->pVertexBindingDescriptions : NULL,
        attrs, attrs ? p->pVertexInputState->pVertexAttributeDescriptions : NULL);
    out->dynamic_input = !p->pVertexInputState;
    out->topology = p->pInputAssemblyState->topology;
    out->static_viewport = !!p->pViewportState->pViewports;
    out->static_scissor = !!p->pViewportState->pScissors;
    if (out->static_viewport) out->viewport = p->pViewportState->pViewports[0];
    if (out->static_scissor) out->scissor = p->pViewportState->pScissors[0];
    const VkPipelineRasterizationStateCreateInfo *r = p->pRasterizationState;
    out->depth_clamp = r->depthClampEnable; out->discard = r->rasterizerDiscardEnable;
    out->polygon = r->polygonMode; out->cull = r->cullMode; out->front_face = r->frontFace;
    out->depth_bias = r->depthBiasEnable; out->bias_constant = r->depthBiasConstantFactor;
    out->bias_clamp = r->depthBiasClamp; out->bias_slope = r->depthBiasSlopeFactor;
    out->line_width = r->lineWidth;
    out->has_depth = !!p->pDepthStencilState;
    if (out->has_depth) {
        const VkPipelineDepthStencilStateCreateInfo *d = p->pDepthStencilState;
        out->depth_test = d->depthTestEnable; out->depth_write = d->depthWriteEnable;
        out->depth_compare = d->depthCompareOp; out->stencil_test = d->stencilTestEnable;
        out->front = d->front; out->back = d->back;
    }
    out->has_color = p->pColorBlendState->attachmentCount;
    out->logic_op = p->pColorBlendState->logicOp;
    if (out->has_color) out->blend = p->pColorBlendState->pAttachments[0];
    memcpy(out->blend_constants, p->pColorBlendState->blendConstants, sizeof(out->blend_constants));
    out->dynamic_count = dynamic;
    if (dynamic) memcpy(out->dynamic_states, p->pDynamicState->pDynamicStates, dynamic * sizeof(VkDynamicState));
    out->set_count = layout->setLayoutCount; out->push_count = layout->pushConstantRangeCount;
    if (out->push_count) memcpy(out->push_ranges, layout->pPushConstantRanges, out->push_count * sizeof(VkPushConstantRange));
    out->color_format = color; out->depth_format = depth;
    return true;
}

static inline bool nv2a_vk_recipe_valid(const NV2AVkPipelineRecipe *r,
                                       const VkPhysicalDeviceLimits *limits)
{
    if (r->flags & ~VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT ||
        (r->stage_count != 2 && r->stage_count != 3) ||
        r->stages[0] != VK_SHADER_STAGE_VERTEX_BIT ||
        r->stages[r->stage_count - 1] != VK_SHADER_STAGE_FRAGMENT_BIT ||
        (r->stage_count == 3 && r->stages[1] != VK_SHADER_STAGE_GEOMETRY_BIT) ||
        r->input.binding_count > 16 || r->input.attribute_count > 16 || r->dynamic_count > 20 ||
        r->set_count > 2 || r->push_count > 2 || r->has_depth > 1 || r->has_color > 1 ||
        r->dynamic_input > 1 || r->static_viewport > 1 || r->static_scissor > 1 ||
        r->depth_clamp > 1 || r->discard > 1 || r->depth_bias > 1 ||
        r->depth_test > 1 || r->depth_write > 1 || r->stencil_test > 1 ||
        r->polygon > VK_POLYGON_MODE_POINT || r->cull > 3 || r->front_face > 1 ||
        r->depth_compare > VK_COMPARE_OP_ALWAYS || r->logic_op > VK_LOGIC_OP_SET ||
        (r->topology != VK_PRIMITIVE_TOPOLOGY_POINT_LIST &&
         r->topology != VK_PRIMITIVE_TOPOLOGY_LINE_LIST &&
         r->topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST) ||
        !isfinite(r->line_width) || r->line_width <= 0 ||
        !isfinite(r->bias_constant) || !isfinite(r->bias_clamp) || !isfinite(r->bias_slope) ||
        r->has_color != (r->color_format != VK_FORMAT_UNDEFINED) ||
        r->has_depth != (r->depth_format != VK_FORMAT_UNDEFINED)) return false;
    bool viewport = false, scissor = false, input = false;
    for (uint32_t i = 0; i < r->dynamic_count; i++) {
        if (!nv2a_vk_recipe_dynamic_state(r->dynamic_states[i])) return false;
        for (uint32_t j = 0; j < i; j++) if (r->dynamic_states[j] == r->dynamic_states[i]) return false;
        viewport |= r->dynamic_states[i] == VK_DYNAMIC_STATE_VIEWPORT;
        scissor |= r->dynamic_states[i] == VK_DYNAMIC_STATE_SCISSOR;
        input |= r->dynamic_states[i] == VK_DYNAMIC_STATE_VERTEX_INPUT_EXT;
    }
    if (r->static_viewport && (!isfinite(r->viewport.x) || !isfinite(r->viewport.y) ||
        !isfinite(r->viewport.width) || !isfinite(r->viewport.height) ||
        r->viewport.width <= 0 || !r->viewport.height ||
        !isfinite(r->viewport.minDepth) || !isfinite(r->viewport.maxDepth) ||
        r->viewport.minDepth < 0 || r->viewport.maxDepth > 1)) return false;
    if ((!r->static_viewport && !viewport) || (!r->static_scissor && !scissor) ||
        (r->dynamic_input && (!input || r->input.binding_count || r->input.attribute_count))) return false;
    for (uint32_t i = 0; i < r->input.binding_count; i++) {
        const VkVertexInputBindingDescription *b = &r->input.bindings[i];
        if (b->binding >= 16 || b->stride > limits->maxVertexInputBindingStride || (uint32_t)b->inputRate > 1) return false;
        for (uint32_t j = 0; j < i; j++) if (b->binding == r->input.bindings[j].binding) return false;
    }
    for (uint32_t i = 0; i < r->input.attribute_count; i++) {
        const VkVertexInputAttributeDescription *a = &r->input.attributes[i];
        if (a->location >= 16 || a->binding >= 16 || !a->format ||
            (uint32_t)a->format > VK_FORMAT_ASTC_12x12_SRGB_BLOCK || a->offset > limits->maxVertexInputAttributeOffset) return false;
        bool bound = false;
        for (uint32_t j = 0; j < r->input.binding_count; j++) bound |= a->binding == r->input.bindings[j].binding;
        if (!bound) return false;
        for (uint32_t j = 0; j < i; j++) if (a->location == r->input.attributes[j].location) return false;
    }
    const VkStencilOpState *stencil[] = {&r->front, &r->back};
    for (unsigned i = 0; i < 2; i++) if ((uint32_t)stencil[i]->failOp > 7 || (uint32_t)stencil[i]->passOp > 7 ||
        (uint32_t)stencil[i]->depthFailOp > 7 || (uint32_t)stencil[i]->compareOp > 7) return false;
    if (r->blend.blendEnable > 1 || r->blend.colorWriteMask > 15 ||
        (uint32_t)r->blend.srcColorBlendFactor > 18 || (uint32_t)r->blend.dstColorBlendFactor > 18 ||
        (uint32_t)r->blend.srcAlphaBlendFactor > 18 || (uint32_t)r->blend.dstAlphaBlendFactor > 18 ||
        (uint32_t)r->blend.colorBlendOp > 4 || (uint32_t)r->blend.alphaBlendOp > 4) return false;
    for (unsigned i = 0; i < 4; i++) if (!isfinite(r->blend_constants[i])) return false;
    for (uint32_t i = 0; i < r->push_count; i++) {
        const VkPushConstantRange *p = &r->push_ranges[i];
        if (!p->stageFlags || (p->stageFlags & ~(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)) ||
            !p->size || (p->size | p->offset) & 3 || p->offset > limits->maxPushConstantsSize ||
            p->size > limits->maxPushConstantsSize - p->offset) return false;
        for (uint32_t j = 0; j < i; j++) if (p->stageFlags & r->push_ranges[j].stageFlags) return false;
    }
    return true;
}

static inline void nv2a_vk_recipe_rebuild(const NV2AVkPipelineRecipe *r,
    const VkShaderModule modules[3], VkPipelineLayout layout, VkRenderPass pass,
    NV2AVkPipelineReplayInfo *out)
{
    memset(out, 0, sizeof(*out));
    for (uint32_t i = 0; i < r->stage_count; i++) out->stages[i] = (VkPipelineShaderStageCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = r->stages[i], .module = modules[i], .pName = "main"};
    out->input = (VkPipelineVertexInputStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = r->input.binding_count, .pVertexBindingDescriptions = r->input.bindings,
        .vertexAttributeDescriptionCount = r->input.attribute_count, .pVertexAttributeDescriptions = r->input.attributes};
    out->assembly = (VkPipelineInputAssemblyStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = r->topology};
    out->viewport = (VkPipelineViewportStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = r->static_viewport ? &r->viewport : NULL,
        .scissorCount = 1, .pScissors = r->static_scissor ? &r->scissor : NULL};
    out->raster = (VkPipelineRasterizationStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = r->depth_clamp, .rasterizerDiscardEnable = r->discard, .polygonMode = r->polygon,
        .cullMode = r->cull, .frontFace = r->front_face, .depthBiasEnable = r->depth_bias,
        .depthBiasConstantFactor = r->bias_constant, .depthBiasClamp = r->bias_clamp,
        .depthBiasSlopeFactor = r->bias_slope, .lineWidth = r->line_width};
    out->depth = (VkPipelineDepthStencilStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = r->depth_test, .depthWriteEnable = r->depth_write,
        .depthCompareOp = r->depth_compare, .stencilTestEnable = r->stencil_test, .front = r->front, .back = r->back};
    out->samples = (VkPipelineMultisampleStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    out->blend = (VkPipelineColorBlendStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOp = r->logic_op, .attachmentCount = r->has_color, .pAttachments = r->has_color ? &r->blend : NULL};
    memcpy(out->blend.blendConstants, r->blend_constants, sizeof(r->blend_constants));
    out->dynamic = (VkPipelineDynamicStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = r->dynamic_count, .pDynamicStates = r->dynamic_states};
    out->pipeline = (VkGraphicsPipelineCreateInfo){.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .flags = r->flags, .stageCount = r->stage_count, .pStages = out->stages,
        .pVertexInputState = r->dynamic_input ? NULL : &out->input, .pInputAssemblyState = &out->assembly,
        .pViewportState = &out->viewport, .pRasterizationState = &out->raster, .pMultisampleState = &out->samples,
        .pDepthStencilState = r->has_depth ? &out->depth : NULL, .pColorBlendState = &out->blend,
        .pDynamicState = r->dynamic_count ? &out->dynamic : NULL, .layout = layout, .renderPass = pass};
}

static inline bool nv2a_vk_recipe_spirv_valid(const uint32_t *words, size_t bytes,
                                            uint32_t stage)
{
    if (bytes < 20 || bytes > NV2A_VK_RECIPE_SHADER_MAX || bytes % 4 ||
        words[0] != 0x07230203 || !words[3] || words[3] > (1u << 24) || words[4]) return false;
    uint32_t model = stage == VK_SHADER_STAGE_VERTEX_BIT ? 0 : stage == VK_SHADER_STAGE_GEOMETRY_BIT ? 3 : 4;
    bool main = false;
    for (size_t i = 5; i < bytes / 4;) {
        uint32_t count = words[i] >> 16, op = words[i] & 0xffff;
        if (!count || count > bytes / 4 - i) return false;
        if (op == 15 && count >= 5 && words[i + 1] == model && words[i + 2] < words[3]) {
            const char *name = (const char *)&words[i + 3];
            size_t capacity = (count - 3) * 4;
            if (capacity >= 5 && !memcmp(name, "main\0", 5)) main = true;
        }
        i += count;
    }
    return main;
}

#endif
