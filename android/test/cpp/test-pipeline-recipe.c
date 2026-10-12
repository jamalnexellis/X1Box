/* Round-trip the real recipe helper and reject torn/corrupt cache records. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <glib.h>
#include "hw/xbox/nv2a/pgraph/vk/pipeline-recipe-file.h"

static void sha(const void *data, size_t size, uint8_t out[32])
{
    GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(sum, data, size);
    gsize length = 32; g_checksum_get_digest(sum, out, &length);
    g_checksum_free(sum);
}

int main(void)
{
    VkPhysicalDeviceLimits limits = {.maxVertexInputBindingStride = 4096,
        .maxVertexInputAttributeOffset = 4095, .maxPushConstantsSize = 256};
    unsigned cases = 0;
    NV2AVkPipelineRecipe saved = {0};
    for (unsigned n = 0; n <= 16; n++) for (unsigned trial = 0; trial < 64; trial++) {
        VkVertexInputBindingDescription bindings[16] = {0};
        VkVertexInputAttributeDescription attributes[16] = {0};
        for (unsigned i = 0; i < n; i++) {
            bindings[i] = (VkVertexInputBindingDescription){.binding = i, .stride = (trial + i) * 4};
            attributes[i] = (VkVertexInputAttributeDescription){.location = i, .binding = i,
                .format = VK_FORMAT_R32G32_SFLOAT, .offset = (trial % 4) * 4};
        }
        bool dynamic_input = trial & 1, has_depth = trial & 2, has_color = trial & 4;
        VkShaderModule modules[3] = {(VkShaderModule)(uintptr_t)1,
            (VkShaderModule)(uintptr_t)2, (VkShaderModule)(uintptr_t)3};
        VkPipelineShaderStageCreateInfo stages[3] = {
            {.stage = VK_SHADER_STAGE_VERTEX_BIT, .module = modules[0], .pName = "main"},
            {.stage = trial & 8 ? VK_SHADER_STAGE_GEOMETRY_BIT : VK_SHADER_STAGE_FRAGMENT_BIT,
             .module = modules[1], .pName = "main"},
            {.stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = modules[2], .pName = "main"}};
        VkPipelineVertexInputStateCreateInfo input = {.vertexBindingDescriptionCount = n,
            .pVertexBindingDescriptions = bindings, .vertexAttributeDescriptionCount = n,
            .pVertexAttributeDescriptions = attributes};
        VkPipelineInputAssemblyStateCreateInfo assembly = {.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
        VkPipelineViewportStateCreateInfo viewport = {.viewportCount = 1, .scissorCount = 1};
        VkPipelineRasterizationStateCreateInfo raster = {.polygonMode = VK_POLYGON_MODE_FILL,
            .lineWidth = 1, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE};
        VkPipelineDepthStencilStateCreateInfo depth = {.depthTestEnable = VK_TRUE,
            .depthWriteEnable = VK_TRUE, .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL};
        VkPipelineMultisampleStateCreateInfo samples = {.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
        VkPipelineColorBlendAttachmentState attachment = {.colorWriteMask = 15};
        VkPipelineColorBlendStateCreateInfo blend = {.attachmentCount = has_color,
            .pAttachments = &attachment, .logicOp = VK_LOGIC_OP_COPY,
            .blendConstants = {.25f, .5f, .75f, 1.f}};
        VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
            VK_DYNAMIC_STATE_VERTEX_INPUT_EXT};
        VkPipelineDynamicStateCreateInfo dyn = {.dynamicStateCount = dynamic_input ? 3 : 2,
            .pDynamicStates = states};
        VkPushConstantRange pushes[] = {{VK_SHADER_STAGE_VERTEX_BIT, 0, 16},
                                       {VK_SHADER_STAGE_FRAGMENT_BIT, 16, 16}};
        VkPipelineLayoutCreateInfo layout = {.setLayoutCount = 2,
            .pushConstantRangeCount = trial & 16 ? 2 : 0, .pPushConstantRanges = pushes};
        VkGraphicsPipelineCreateInfo info = {.stageCount = trial & 8 ? 3 : 2, .pStages = stages,
            .flags = trial & 32 ? VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT : 0,
            .pVertexInputState = dynamic_input ? NULL : &input, .pInputAssemblyState = &assembly,
            .pViewportState = &viewport, .pRasterizationState = &raster, .pDepthStencilState = has_depth ? &depth : NULL,
            .pMultisampleState = &samples, .pColorBlendState = &blend, .pDynamicState = &dyn};
        NV2AVkPipelineRecipe original, roundtrip;
        assert(nv2a_vk_recipe_capture(&original, &info, &layout,
            has_color ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_UNDEFINED,
            has_depth ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_UNDEFINED));
        assert(nv2a_vk_recipe_valid(&original, &limits));
        memset(bindings, 0xcc, sizeof(bindings)); memset(attributes, 0xdd, sizeof(attributes));
        memset(stages, 0xee, sizeof(stages)); memset(states, 0xff, sizeof(states));
        NV2AVkPipelineReplayInfo replay;
        nv2a_vk_recipe_rebuild(&original, modules, VK_NULL_HANDLE, VK_NULL_HANDLE, &replay);
        VkPipelineLayoutCreateInfo restored_layout = {.setLayoutCount = original.set_count,
            .pushConstantRangeCount = original.push_count, .pPushConstantRanges = original.push_ranges};
        assert(nv2a_vk_recipe_capture(&roundtrip, &replay.pipeline, &restored_layout,
            original.color_format, original.depth_format));
        assert(!memcmp(&original, &roundtrip, sizeof(original)));
        saved = original;
        cases++;
    }
    assert(cases == 1088);
    NV2AVkPipelineRecipe bad = saved; bad.stage_count = UINT32_MAX;
    assert(!nv2a_vk_recipe_valid(&bad, &limits));
    bad = saved; bad.input.attribute_count = 17;
    assert(!nv2a_vk_recipe_valid(&bad, &limits));
    bad = saved; bad.dynamic_count = 21;
    assert(!nv2a_vk_recipe_valid(&bad, &limits));
    bad = saved; bad.line_width = NAN;
    assert(!nv2a_vk_recipe_valid(&bad, &limits));
    bad = saved; bad.push_count = 3;
    assert(!nv2a_vk_recipe_valid(&bad, &limits));
    NV2AVkRecipeHeader header = {.magic = NV2A_VK_RECIPE_MAGIC, .abi = NV2A_VK_RECIPE_ABI,
        .record_size = sizeof(NV2AVkRecipeRecord)};
    NV2AVkRecipeRecord record = {.recipe = saved}, decoded;
    sha(&record.recipe, sizeof(record.recipe), record.digest);
    size_t length = sizeof(header) + sizeof(record);
    uint8_t *storage = calloc(1, length + sizeof(record) + 1); assert(storage);
    uint8_t *bytes = storage + 1; /* On-disk buffers need not be aligned. */
    memcpy(bytes, &header, sizeof(header)); memcpy(bytes + sizeof(header), &record, sizeof(record));
    assert(nv2a_vk_recipe_file_record(bytes, length, &header, 0, sha, &decoded));
    assert(!memcmp(&decoded, &record, sizeof(record)));
    for (size_t size = 0; size < length; size++)
        assert(!nv2a_vk_recipe_file_record(bytes, size, &header, 0, sha, &decoded));
    assert(nv2a_vk_recipe_file_record(bytes, length + 17, &header, 0, sha, &decoded));
    assert(!nv2a_vk_recipe_file_record(bytes, length + 17, &header, 1, sha, &decoded));
    assert(!nv2a_vk_recipe_file_record(bytes, length, &header, SIZE_MAX, sha, &decoded));
    for (size_t i = 0; i < length; i++) {
        bytes[i] ^= 1;
        assert(!nv2a_vk_recipe_file_record(bytes, length, &header, 0, sha, &decoded));
        bytes[i] ^= 1;
    }
    assert(!nv2a_vk_recipe_file_valid(bytes,
        sizeof(header) + (NV2A_VK_RECIPE_MAX + 1) * sizeof(record), &header));
    free(storage);
    uint32_t spirv[] = {0x07230203, 0x10000, 0, 2, 0, (5u << 16) | 15, 0, 1, 0x6e69616d, 0};
    assert(nv2a_vk_recipe_spirv_valid(spirv, sizeof(spirv), VK_SHADER_STAGE_VERTEX_BIT));
    assert(!nv2a_vk_recipe_spirv_valid(spirv, sizeof(spirv), VK_SHADER_STAGE_FRAGMENT_BIT));
    spirv[5] = 15;
    assert(!nv2a_vk_recipe_spirv_valid(spirv, sizeof(spirv), VK_SHADER_STAGE_VERTEX_BIT));
    printf("PASS recipe round trips=%u; corrupt/truncated byte cases=%zu; SPIR-V bounds/stage checks\n", cases, length * 2);
    return 0;
}
